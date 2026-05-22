#include "Pager.h"
#include <iostream>
#include <stdexcept>
#include <filesystem>

namespace fs = std::filesystem;

std::streampos Pager::get_page_offset(uint32_t page_id) const {
    return static_cast<std::streampos>(page_id) * PAGE_SIZE;
}

Pager::Pager(const std::filesystem::path& db_path) : filename(db_path), page_count(0) {

    if (db_path.empty()) {
        throw std::invalid_argument("Pager Error: Database path cannot be empty.");
    }

    // Логика: пытаемся открыть существующий файл для чтения и записи в бинарном режиме
    file_stream.open(filename, std::ios::in | std::ios::out | std::ios::binary);

    if (!file_stream.is_open()) {
        file_stream.clear();

        // Логика: если файла нет, создаем его (trunc)
        file_stream.open(filename, std::ios::out | std::ios::binary | std::ios::trunc);
        
        if (!file_stream.is_open()) {
            throw std::runtime_error("Pager Fatal: Could not create table file: " + filename.string());
        }
        
        file_stream.close(); 
        file_stream.open(filename, std::ios::in | std::ios::out | std::ios::binary);

        // Логика: выделяем первую страницу (Page 0) под заголовок таблицы
        allocate_page(); 
    }

    if (!file_stream.is_open()) {
        throw std::runtime_error("Pager Fatal: Failed to establish file stream for: " + filename.string());
    }

    // Логика: определяем размер файла и количество страниц
    file_stream.seekg(0, std::ios::end);
    std::streampos file_size = file_stream.tellg();
    
    page_count = static_cast<uint32_t>(file_size / PAGE_SIZE);

}

Pager::~Pager() {
    if (file_stream.is_open()) {
        file_stream.flush();
        file_stream.close();
    }
}

Result Pager::read_page(uint32_t page_id, void* buffer) {
    // Логика проверки границ файла
    if (page_id >= page_count) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Out-of-bounds (ID: " + std::to_string(page_id) + ")");
    }

    file_stream.clear();

    // Перемещаем указатель в файле на нужную страницу
    file_stream.seekg(get_page_offset(page_id), std::ios::beg);
    
    // Считываем ровно PAGE_SIZE байт
    file_stream.read(static_cast<char*>(buffer), PAGE_SIZE);

    // Проверка: удалось ли физически прочитать данные с диска
    if (file_stream.fail()) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Physical read failed (ID: " + std::to_string(page_id) + ")");
    }

    return Result::Success();
}

Result Pager::write_page(uint32_t page_id, const void* buffer) {
    // Логика: нельзя писать в страницу, которой нет в файле
    if (page_id >= page_count) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Cannot write to unallocated page " + std::to_string(page_id));
    }

    file_stream.clear();

    // Перемещаем указатель для записи (put pointer) на нужный офсет
    file_stream.seekp(get_page_offset(page_id), std::ios::beg);
    
    // Записываем ровно PAGE_SIZE байт
    file_stream.write(static_cast<const char*>(buffer), PAGE_SIZE);

    // Проверка: удалось ли физически записать данные на диск
    if (file_stream.fail()) {
        return Result::Error(StatusCode::IO_ERROR, "Pager Error: Physical write failed at page " + std::to_string(page_id));
    }

    // Принудительно выталкиваем данные из буфера ОС на физический диск
    file_stream.flush();

    return Result::Success();
}

uint32_t Pager::allocate_page() {
    file_stream.clear();

    file_stream.seekp(0, std::ios::end);
    std::vector<char> buffer(PAGE_SIZE, 0); 
    file_stream.write(buffer.data(), PAGE_SIZE);
    
    if (file_stream.fail()) {
        throw std::runtime_error("Pager Fatal: Failed to extend file on disk.");
    }
    file_stream.flush();

    uint32_t new_page_id = page_count;
    page_count++; 
    
    return new_page_id;
}
