#include "kimi_authoring.h"
#include "kimi_config.h"

#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>
#include <exception>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace kimi {

int run_authoring_print(int argc, char** argv)
{
    namespace addon = logonomic::cli_addon;
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3 && args.size() != 5) return 2;
    if (args.size() == 5 && args[3] != QStringLiteral("-m") && args[3] != QStringLiteral("--model")) return 2;
    const auto agent = QDir(app.applicationDirPath()).absoluteFilePath(QStringLiteral("../assets/kimi-authoring.md"));
    QTemporaryDir scratch(QDir::current().absoluteFilePath(QStringLiteral("kimi-authoring-XXXXXX")));
    if (!QFileInfo::exists(agent) || !scratch.isValid()) return 2;

    // Native print mode takes its prompt only as an argument. Receive it from
    // the host over stdin, then pass one literal Unicode argument to Kimi.
    QFile input;
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return 2;
#endif
    if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    QByteArray bytes;
    for (;;) {
        const auto part = input.read(8192);
        if (input.error() != QFileDevice::NoError) return 2;
        if (part.isEmpty()) break;
        bytes += part;
        if (bytes.size() > static_cast<qsizetype>(addon::limits::k_max_structured_prompt_bytes)) return 2;
    }
    if (bytes.isEmpty()) return 2;
    try {
        if (authoring_profile_restriction(addon::path_from_utf8(qEnvironmentVariable("KIMI_CODE_HOME").toStdString()),
                addon::path_from_utf8(scratch.path().toStdString()))) return 2;
    }
    catch (const std::exception&) {
        return 2;
    }
    auto options = args.mid(3);
    options << QStringLiteral("--agent-file") << agent << QStringLiteral("--skills-dir") << scratch.path()
            << QStringLiteral("--output-format") << QStringLiteral("stream-json")
            << QStringLiteral("--prompt") << QStringLiteral("User request:\n") + QString::fromUtf8(bytes);
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setWorkingDirectory(scratch.path());
    process.start(args[2], options);
    if (!process.waitForStarted() || !process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit) return 3;
    return process.exitCode();
}

} // namespace kimi
