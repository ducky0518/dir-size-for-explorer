#pragma once

#include <Windows.h>

#include "dirsize/db.h"
#include "dirsize/ipc.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace dirsize {

class Scanner;

class IpcServer {
public:
    explicit IpcServer(std::shared_ptr<Database> db);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    // Set the scanner reference (for forwarding recalculate/reload commands).
    void SetScanner(Scanner* scanner) { m_scanner = scanner; }

    // Optional hook invoked after a ReloadConfig command, so the host can
    // apply config changes beyond the scanner (e.g. restart the directory
    // watchers when the watched-directory list changed).
    void SetReloadCallback(std::function<void()> cb) { m_onReload = std::move(cb); }

    void Start();
    void Stop();

private:
    void ListenerThread();
    void HandleClient(HANDLE hPipe);

    std::shared_ptr<Database> m_db;
    Scanner* m_scanner = nullptr;
    std::function<void()> m_onReload;

    std::thread m_listenerThread;
    std::atomic<bool> m_running{false};
    HANDLE m_stopEvent = nullptr;
};

} // namespace dirsize
