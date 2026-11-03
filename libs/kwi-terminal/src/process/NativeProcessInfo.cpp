// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "NativeProcessInfo_p.h"
#include "ProcessQueryLimits_p.h"

#ifdef Q_OS_MACOS
#include "DarwinProcessArguments_p.h"
#include <libproc.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>
#elif defined(Q_OS_FREEBSD)
#include <cstddef>
#include <limits.h>
#include <span>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <vector>
#else
#include <QDirListing>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <unistd.h>
#endif

namespace Kwi::NativeProcessInfo
{
#ifdef Q_OS_MACOS
QString directory(pid_t pid)
{
    proc_vnodepathinfo info{};

    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &info, sizeof(info)) != sizeof(info)) {
        return {};
    }

    return QString::fromLocal8Bit(info.pvi_cdir.vip_path, strnlen(info.pvi_cdir.vip_path, sizeof(info.pvi_cdir.vip_path)));
}

QString executable(pid_t pid)
{
    char path[PROC_PIDPATHINFO_MAXSIZE]{};

    if (pid <= 0 || proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return {};
    }

    return QString::fromLocal8Bit(path, strnlen(path, sizeof(path)));
}

QByteArray commandLine(pid_t pid)
{
    // KERN_PROCARGS2 includes the executable path and environment as well as argv.
    // Query its bounded kernel capacity, then return only the normalized argv.
    int limit{};
    size_t size{sizeof(limit)};
    const int limitMib[]{CTL_KERN, KERN_ARGMAX};

    if (pid <= 0 || sysctl(const_cast<int *>(limitMib), 2, &limit, &size, nullptr, 0) != 0 || size != sizeof(limit) || limit <= 0 || limit > 2 * 1024 * 1024) {
        return {};
    }

    QByteArray bytes(limit, Qt::Uninitialized);
    size = size_t(bytes.size());
    int mib[]{CTL_KERN, KERN_PROCARGS2, pid};

    if (sysctl(mib, 3, bytes.data(), &size, nullptr, 0) != 0 || size > size_t(bytes.size())) {
        return {};
    }

    return darwinArguments(QByteArrayView{bytes.constData(), qsizetype(size)});
}

quint64 startTime(pid_t pid)
{
    proc_bsdinfo info{};

    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) {
        return 0;
    }

    return info.pbi_start_tvsec * 1000000 + info.pbi_start_tvusec;
}

pid_t groupMember(pid_t group, pid_t session)
{
    if (group <= 0 || session <= 0) {
        return -1;
    }

    // Only enumerate this group, and only when its leader has gone. Bound both
    // allocation and races with group growth; never scan all PIDs on the UI path.
    constexpr int maximumMembers{16384};
    const int bytes{proc_listpids(PROC_PGRP_ONLY, uint32_t(group), nullptr, 0)};

    if (bytes <= 0 || bytes > int(sizeof(pid_t)) * maximumMembers) {
        return -1;
    }

    const int capacity{qMin(maximumMembers, bytes / int(sizeof(pid_t)) + 32)};
    std::vector<pid_t> members(capacity);
    const int used{proc_listpids(PROC_PGRP_ONLY, uint32_t(group), members.data(), capacity * int(sizeof(pid_t)))};

    if (used <= 0 || used >= capacity * int(sizeof(pid_t))) {
        return -1;
    }

    for (int i{0}; i < used / int(sizeof(pid_t)); ++i) {
        if (members[i] > 0 && getpgid(members[i]) == group && getsid(members[i]) == session) {
            return members[i];
        }
    }

    return -1;
}
#elif defined(Q_OS_FREEBSD)
namespace
{
bool query(pid_t pid, int field, void *data, size_t &size)
{
    const int mib[]{CTL_KERN, KERN_PROC, field, pid};

    return sysctl(mib, 4, data, &size, nullptr, 0) == 0;
}
}

QString directory(pid_t pid)
{
    // KERN_PROC_CWD returns a packed kinfo_file.
    kinfo_file file{};
    size_t size{sizeof(file)};

    if (!query(pid, KERN_PROC_CWD, &file, size) || size <= offsetof(kinfo_file, kf_path)) {
        return {};
    }

    return QString::fromLocal8Bit(file.kf_path);
}

QString executable(pid_t pid)
{
    char path[PATH_MAX]{};
    size_t size{sizeof(path)};

    return query(pid, KERN_PROC_PATHNAME, path, size) ? QString::fromLocal8Bit(path) : QString{};
}

QByteArray commandLine(pid_t pid)
{
    QByteArray bytes(ProcessQueryLimits::MaximumCommandLineBytes + 1, Qt::Uninitialized);
    size_t size{size_t(bytes.size())};

    if (!query(pid, KERN_PROC_ARGS, bytes.data(), size)) {
        return {};
    }

    bytes.truncate(qsizetype(size));

    return bytes;
}

quint64 startTime(pid_t pid)
{
    kinfo_proc process{};
    size_t size{sizeof(process)};

    if (!query(pid, KERN_PROC_PID, &process, size) || size != sizeof(process)) {
        return 0;
    }

    return quint64(process.ki_start.tv_sec) * 1000000 + quint64(process.ki_start.tv_usec);
}

pid_t groupMember(pid_t group, pid_t session)
{
    size_t size{0};

    if (!query(group, KERN_PROC_PGRP, nullptr, size) || size == 0) {
        return -1;
    }

    std::vector<kinfo_proc> processes(size / sizeof(kinfo_proc));
    size = processes.size() * sizeof(kinfo_proc);

    if (!query(group, KERN_PROC_PGRP, processes.data(), size)) {
        return -1;
    }

    for (const auto &process : std::span{processes.data(), size / sizeof(kinfo_proc)}) {
        if (process.ki_pgid == group && process.ki_sid == session) {
            return process.ki_pid;
        }
    }

    return -1;
}
#else
QString directory(pid_t pid)
{
    return QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget();
}

QString executable(pid_t pid)
{
    return QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
}

QByteArray commandLine(pid_t pid)
{
    QFile file{QStringLiteral("/proc/%1/cmdline").arg(pid)};

    return file.open(QIODevice::ReadOnly) ? file.read(ProcessQueryLimits::MaximumCommandLineBytes + 1) : QByteArray{};
}

quint64 startTime(pid_t pid)
{
    QFile file{QStringLiteral("/proc/%1/stat").arg(pid)};
    const QByteArray bytes{file.open(QIODevice::ReadOnly) ? file.read(4096) : QByteArray{}};
    const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).split(' ');

    return fields.size() > 19 ? fields[19].toULongLong() : 0;
}

pid_t groupMember(pid_t group, pid_t session)
{
    if (group <= 0 || session <= 0) {
        return -1;
    }

    for (const auto &entry : QDirListing{QStringLiteral("/proc"), QDirListing::IteratorFlag::DirsOnly}) {
        bool valid{false};
        const int pid{entry.fileName().toInt(&valid)};

        if (valid && getpgid(pid) == group && getsid(pid) == session) {
            return pid;
        }
    }

    return -1;
}
#endif
}
