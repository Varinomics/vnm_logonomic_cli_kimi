#!/usr/bin/env python3
"""Check PE closure reuse against exact bytes and DLL search membership."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def cmake_string(value: Path) -> str:
    return '[==[' + value.as_posix() + ']==]'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('module', 'cmake', 'inspector', 'binary', 'qt-runtime'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    require(os.name == 'nt', 'this fixture exercises the Windows PE resolver')
    with tempfile.TemporaryDirectory(prefix='kimi-runtime-cache-') as scratch:
        root = Path(scratch)
        origin = root / 'origin'
        runtime_dir = root / 'runtime'
        cache_dir = root / 'cache'
        origin.mkdir()
        runtime_dir.mkdir()
        binary = origin / Path(args.binary).name
        runtime = runtime_dir / Path(args.qt_runtime).name
        shutil.copyfile(args.binary, binary)
        shutil.copyfile(args.qt_runtime, runtime)
        script = root / 'resolve.cmake'
        result = root / 'resolved.txt'
        script.write_text('\n'.join([
            'cmake_minimum_required(VERSION 3.25)',
            'set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)',
            'set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL dumpbin)',
            f'set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND {cmake_string(Path(args.inspector))})',
            f'set(runtime_search_dirs {cmake_string(runtime_dir)})',
            'set(exclusions PRE_EXCLUDE_REGEXES "^[Aa][Pp][Ii]-" "^[Ee][Xx][Tt]-"',
            '    POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*")',
            f'set(kimi_runtime_dependency_cache_directory {cmake_string(cache_dir)})',
            f'include({cmake_string(Path(args.module))})',
            f'kimi_resolve_runtime_dependencies(resolved EXECUTABLES {cmake_string(binary)})',
            f'file(WRITE {cmake_string(result)} "${{resolved}}")',
        ]) + '\n', encoding='utf-8')

        def resolve(success: bool = True) -> tuple[str, float]:
            started = time.perf_counter()
            process = subprocess.run([args.cmake, '-P', str(script)], text=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     encoding='utf-8', errors='replace', timeout=120)
            elapsed = time.perf_counter() - started
            require((process.returncode == 0) == success, process.stdout)
            return (result.read_text(encoding='utf-8') if success else process.stdout, elapsed)

        resolved, cold_seconds = resolve()
        require(runtime.as_posix() in resolved, 'the selected external Qt runtime was not resolved')
        caches = list(cache_dir.glob('*.json'))
        require(len(caches) == 1, 'the PE closure cache was not populated')
        cache = caches[0]
        first = cache.read_bytes()
        stamp = cache.stat().st_mtime_ns
        _, warm_seconds = resolve()
        require(cache.read_bytes() == first and cache.stat().st_mtime_ns == stamp,
                'an unchanged closure was rediscovered instead of reused')

        # A valid PE image can contain an overlay. Changing it with its original
        # timestamp proves content invalidation independently of filesystem time.
        original_runtime = runtime.read_bytes()
        runtime_stamp = runtime.stat()
        runtime.write_bytes(original_runtime + b'content invalidation oracle\n')
        os.utime(runtime, ns=(runtime_stamp.st_atime_ns, runtime_stamp.st_mtime_ns))
        resolve()
        changed = cache.read_bytes()
        require(changed != first, 'backdated external runtime replacement reused the old closure')
        runtime.write_bytes(original_runtime)
        os.utime(runtime, ns=(runtime_stamp.st_atime_ns, runtime_stamp.st_mtime_ns))
        resolve()

        preferred = origin / runtime.name
        shutil.copyfile(runtime, preferred)
        os.utime(preferred, (1, 1))
        resolved, _ = resolve()
        require(preferred.as_posix() in resolved and runtime.as_posix() not in resolved,
                'a backdated higher-priority DLL did not change resolution')
        preferred.unlink()
        resolved, _ = resolve()
        require(runtime.as_posix() in resolved, 'removing the preferred DLL did not restore resolution')

        before_binary_change = cache.read_bytes()
        binary_stamp = binary.stat()
        binary.write_bytes(binary.read_bytes() + b'binary invalidation oracle\n')
        os.utime(binary, ns=(binary_stamp.st_atime_ns, binary_stamp.st_mtime_ns))
        resolve()
        require(cache.read_bytes() != before_binary_change,
                'backdated executable replacement reused the old closure')
        cache.write_text('{invalid cache', encoding='utf-8')
        resolve()
        json.loads(cache.read_text(encoding='utf-8'))
        runtime.unlink()
        output, _ = resolve(False)
        require('Incomplete runtime closure' in output,
                'a missing external runtime did not report the actual closure failure')
        runtime.write_bytes(original_runtime)
        alternative_dir = root / 'alternative'
        alternative_dir.mkdir()
        alternative = alternative_dir / runtime.name
        shutil.copyfile(runtime, alternative)
        original_search = f'set(runtime_search_dirs {cmake_string(runtime_dir)})'
        alternative_first = ('set(runtime_search_dirs ' + cmake_string(alternative_dir) +
                             ' ' + cmake_string(runtime_dir) + ')')
        original_first = ('set(runtime_search_dirs ' + cmake_string(runtime_dir) +
                          ' ' + cmake_string(alternative_dir) + ')')
        script.write_text(script.read_text(encoding='utf-8').replace(original_search, alternative_first),
                          encoding='utf-8')
        resolved, _ = resolve()
        require(alternative.as_posix() in resolved, 'a changed search path did not select its first DLL')
        script.write_text(script.read_text(encoding='utf-8').replace(alternative_first, original_first),
                          encoding='utf-8')
        resolved, _ = resolve()
        require(runtime.as_posix() in resolved, 'a changed search order reused the previous DLL')
        print(json.dumps({'cold_seconds': cold_seconds, 'warm_seconds': warm_seconds,
                          'checks': 'no-op, backdated runtime/executable changes, search precedence, '
                                    'search path/order changes, DLL removal, malformed cache, '
                                    'unresolved dependency'}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
