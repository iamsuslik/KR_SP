#include "AsyncManager.h"
#include <random>
#include <shared_mutex>
#include <deque>

static constexpr size_t DEFAULT_THREADS = 4;
static constexpr size_t MAX_THREADS = 16;
static constexpr size_t MAX_REGISTRY_SIZE = 1000;

static std::shared_mutex registry_mtx;
static std::deque<std::string> history_fifo;

AsyncManager::AsyncManager(size_t threads) {
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = DEFAULT_THREADS;
        else if (threads > MAX_THREADS) threads = MAX_THREADS; 
    }

    pool.reserve(threads);
    for (size_t i = 0; i < threads; ++i) pool.emplace_back(&AsyncManager::worker_loop, this);
}

AsyncManager::~AsyncManager() {
    should_stop = true;
    cv.notify_all();
    for (auto& w : pool) if (w.joinable()) w.join();
}

std::string AsyncManager::generate_guid_v4() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    uint32_t d[4] = {dis(gen), (dis(gen) & 0xFFFF0FFF) | 0x4000, (dis(gen) & 0x3FFFFFFF) | 0x80000000, dis(gen)};
    char b[37];
    std::snprintf(b, sizeof(b), "%08x-%04x-%04x-%04x-%08x%04x", d[0], d[1] >> 16, d[1] & 0xFFFF, d[2] >> 16, d[2] & 0xFFFF, d[3]);
    return std::string(b);
}

std::string AsyncManager::enqueue(const std::string& query) {
    std::string guid = generate_guid_v4();
    {
        std::unique_lock lock(registry_mtx);
        if (registry.size() >= MAX_REGISTRY_SIZE) {
            registry.erase(history_fifo.front());
            history_fifo.pop_front();
        }
        registry[guid] = {AsyncStatus::PENDING, "", StatusCode::OK, ""};
        history_fifo.push_back(guid);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        task_queue.push({guid, query});
    }
    cv.notify_one();
    return guid;
}

AsyncResult AsyncManager::fetch_result(const std::string& guid) {
    std::shared_lock lock(registry_mtx);
    return registry.count(guid) ? registry.at(guid) : AsyncResult{AsyncStatus::FAILED, "GUID_NOT_FOUND", StatusCode::NOT_FOUND, ""};
}

void AsyncManager::worker_loop() {
    while (true) {
        AsyncTask task;
        if (!try_get_task(task)) return;

        std::string query = task.query; 
        process_and_finalize(task, query);
    }
}

// ЭТАП 1: Безопасное извлечение задачи из очереди
bool AsyncManager::try_get_task(AsyncTask& task) {
    std::unique_lock<std::mutex> lock(queue_mtx);
    cv.wait(lock, [this] { return should_stop || !task_queue.empty(); });
    
    if (should_stop && task_queue.empty()) return false;
    
    task = std::move(task_queue.front());
    task_queue.pop();
    return true;
}

// ЭТАП 2: Чтение данных
std::string AsyncManager::get_query_text(const AsyncTask& task) {
    return task.query;
}

// ЭТАП 3: Выполнение логики БД и отправка ответа в сокет
void AsyncManager::process_and_finalize(AsyncTask& task, const std::string& query) {
    std::string result_buffer;
    StatusCode sc = StatusCode::OK;

    // Выполняем SQL-запрос через движок
    if (db_engine && !query.empty()) {
        auto output_cb = [&](const std::string& s) { result_buffer += s; };
        sc = db_engine(query, output_cb).code;
    }

    // Просто обновляем реестр, чтобы клиент мог забрать результат по CHECK
    update_registry(task.guid, std::move(result_buffer), sc);
}

// ЭТАП 4: Обновление статуса и времени завершения
void AsyncManager::update_registry(const std::string& guid, std::string buf, StatusCode sc) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char t_buf[26];
    #ifdef _WIN32
        ctime_s(t_buf, sizeof(t_buf), &now);
    #else
        ctime_r(&now, t_buf);
    #endif
    std::string timestamp(t_buf);
    if (!timestamp.empty()) timestamp.pop_back(); // Убираем \n от ctime

    std::unique_lock lock(registry_mtx);
    if (registry.count(guid)) {
        registry[guid] = {AsyncStatus::COMPLETED, std::move(buf), sc, timestamp};
    }
}
