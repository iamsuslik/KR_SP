#include "AsyncManager.h"
#include <random>
#include <shared_mutex>
#include <deque>

static std::shared_mutex registry_mtx;
static std::deque<std::string> history_fifo; // Для строгого соблюдения FIFO при очистке

AsyncManager::AsyncManager(size_t threads) {
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
        if (registry.size() >= 1000) {
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
        
        // 1. Ждем задачу. Если сервер останавливается - выходим.
        if (!try_get_task(task)) return;

        // 2. Получаем текст запроса (из структуры или из сокета)
        std::string query = get_query_text(task);
        if (query.empty() && task.client_fd != -1) {
            close(task.client_fd);
            continue;
        }

        // 3. Выполняем запрос и отправляем ответ
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

// ЭТАП 2: Чтение данных (логика "узкого горлышка" теперь в отдельном потоке)
std::string AsyncManager::get_query_text(const AsyncTask& task) {
    if (task.client_fd != -1) {
        // Здесь поток может заблокироваться на чтении, но это не мешает основному серверу
        return NetworkManager::receiveString(task.client_fd);
    }
    return task.query;
}

// ЭТАП 3: Выполнение логики БД и отправка ответа в сокет
// Исправленный метод в AsyncManager.cpp
void AsyncManager::process_and_finalize(AsyncTask& task, const std::string& query) {
    std::string result_buffer;
    StatusCode sc = StatusCode::OK;

    // 1. ПРОВЕРКА: Это команда CHECK или SQL-запрос?
    if (query.size() > 6 && query.substr(0, 6) == "CHECK ") {
        // Логика CHECK (прямое чтение из реестра)
        std::string target_guid = query.substr(6);
        AsyncResult res = fetch_result(target_guid);
        
        if (res.status == AsyncStatus::COMPLETED) result_buffer = res.data;
        else if (res.status == AsyncStatus::FAILED) result_buffer = "[Error] Task failed.";
        else result_buffer = "[Pending] Still processing...";
    } 
    else {
        // Логика SQL-запроса (через движок БД)
        if (db_engine && !query.empty()) {
            auto output_cb = [&](const std::string& s) { result_buffer += s; };
            sc = db_engine(query, output_cb).code;
        }
    }

    // 2. ОТВЕТ В СЕТЬ
    if (task.client_fd != -1) {
        NetworkManager::sendString(task.client_fd, result_buffer);
        NetworkManager::sendString(task.client_fd, "EOF_MARKER");
        close(task.client_fd);
    }

    // 3. ОБНОВЛЕНИЕ РЕЕСТРА (для истории)
    update_registry(task.guid, std::move(result_buffer), sc);
}

// ЭТАП 4: Обновление статуса и времени завершения
void AsyncManager::update_registry(const std::string& guid, std::string buf, StatusCode sc) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char t_buf[26];
    ctime_r(&now, t_buf);
    std::string timestamp(t_buf);
    if (!timestamp.empty()) timestamp.pop_back(); // Убираем \n от ctime

    std::unique_lock lock(registry_mtx);
    if (registry.count(guid)) {
        registry[guid] = {AsyncStatus::COMPLETED, std::move(buf), sc, timestamp};
    }
}

std::string AsyncManager::enqueue_socket(int fd) {
    std::string guid = generate_guid_v4();
    {
        std::unique_lock lock(registry_mtx);
        registry[guid] = {AsyncStatus::PENDING, "", StatusCode::OK, ""};
        history_fifo.push_back(guid);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        task_queue.push({guid, "", fd}); // Передаем сокет, query пока пустой
    }
    cv.notify_one();
    return guid;
}