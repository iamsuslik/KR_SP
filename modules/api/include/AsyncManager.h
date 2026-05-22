#ifndef ASYNC_MANAGER_H
#define ASYNC_MANAGER_H

#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <shared_mutex> // Для высокопроизводительного чтения результатов
#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <atomic>
#include "common.h"

// Статусы асинхронной обработки
enum class AsyncStatus {
    PENDING,    // Ожидает в очереди
    PROCESSING, // В процессе выполнения
    COMPLETED,  // Выполнено успешно
    FAILED      // Ошибка выполнения
};

// Контейнер результата запроса
struct AsyncResult {
    AsyncStatus status;
    std::string data;           // Тело ответа (JSON)
    StatusCode db_code;         // Код возврата логики БД
    std::string completion_time; // Метка времени завершения
};

class AsyncManager {
private:
    struct AsyncTask {
        std::string guid;
        std::string query;
        std::string query; // Может быть пустым, если мы читаем из сокета
        int client_fd = -1; // Добавили это поле
    };

    // Очередь задач (защищена обычным mutex)
    std::queue<AsyncTask> task_queue;
    std::mutex queue_mtx;
    std::condition_variable cv;

    // Реестр результатов (защищен shared_mutex для параллельного чтения)
    std::unordered_map<std::string, AsyncResult> registry;
    
    std::vector<std::thread> pool;
    std::atomic<bool> should_stop{false};

    // Внутренние системные методы
    std::string generate_guid_v4();
    void worker_loop();

public:
    std::string enqueue_socket(int fd);
    // Колбэк-интерфейс для подключения SQLParser
    using Processor = std::function<Result(const std::string&, std::function<void(const std::string&)>)>;
    Processor db_engine;

    explicit AsyncManager(size_t threads = 4);
    ~AsyncManager();

    // Запрет копирования и перемещения (AsyncManager — уникальный ресурс сервера)
    AsyncManager(const AsyncManager&) = delete;
    AsyncManager& operator=(const AsyncManager&) = delete;

    /**
     * Постановка запроса в очередь. Возвращает уникальный идентификатор (GUID).
     */
    std::string enqueue(const std::string& query);

    /**
     * Получение текущего состояния и результата запроса по его GUID.
     * Потокобезопасно, поддерживает параллельное чтение.
     */
    AsyncResult fetch_result(const std::string& guid);

    /**
     * Очистка всей истории результатов.
     */
    void prune_results();
    bool try_get_task(AsyncTask& task);
    std::string get_query_text(const AsyncTask& task);
    void process_and_finalize(AsyncTask& task, const std::string& query);
    void update_registry(const std::string& guid, std::string buf, StatusCode sc);
};

#endif