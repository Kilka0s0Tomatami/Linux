#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "curse_work.h"

// Путь к смонтированной ФС
static std::string mount_path = "/mnt/curse_work";

// Получение отсортированного списка файлов
std::vector<std::string> get_file_list()
{
    std::vector<std::string> files;
    DIR *dir = opendir(mount_path.c_str());

    if (!dir) {
        std::cerr << "Ошибка: не удалось открыть " << mount_path
                  << ": " << strerror(errno) << std::endl;
        return files;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "file_", 5) == 0)
            files.push_back(entry->d_name);
    }
    closedir(dir);

    // Сортируем по числовому индексу
    std::sort(files.begin(), files.end(),
              [](const std::string &a, const std::string &b) {
                  return std::atoi(a.c_str() + 5) < std::atoi(b.c_str() + 5);
              });

    return files;
}

// Открываем любой файл ФС для вызова IOCTL
int open_any_file()
{
    std::vector<std::string> files = get_file_list();
    if (files.empty()) {
        std::cerr << "Нет файлов в " << mount_path << std::endl;
        return -1;
    }

    std::string path = mount_path + "/" + files[0];
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0)
        std::cerr << "Ошибка открытия " << path << ": " << strerror(errno) << std::endl;
    return fd;
}

// Тест: запись случайного uint32_t в каждый файл, затем чтение и сравнение
void cmd_test()
{
    std::vector<std::string> files = get_file_list();
    if (files.empty()) {
        std::cerr << "Нет файлов для тестирования" << std::endl;
        return;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, UINT32_MAX);

    int success = 0, fail = 0;
    std::cout << "Тестирование записи/чтения " << files.size() << " файлов..." << std::endl;

    for (const auto &filename : files) {
        std::string filepath = mount_path + "/" + filename;
        uint32_t write_val = dist(gen);
        uint32_t read_val = 0;

        // Открываем файл
        int fd = open(filepath.c_str(), O_RDWR);
        if (fd < 0) {
            std::cerr << "  [FAIL] open(" << filename << "): " << strerror(errno) << std::endl;
            fail++;
            continue;
        }

        // Пишем случайное число в начало
        ssize_t w = write(fd, &write_val, sizeof(write_val));
        if (w != sizeof(write_val)) {
            std::cerr << "  [FAIL] write(" << filename << "): " << strerror(errno) << std::endl;
            close(fd);
            fail++;
            continue;
        }

        // Перемещаемся в начало и читаем обратно
        lseek(fd, 0, SEEK_SET);
        ssize_t r = read(fd, &read_val, sizeof(read_val));
        if (r != sizeof(read_val)) {
            std::cerr << "  [FAIL] read(" << filename << "): " << strerror(errno) << std::endl;
            close(fd);
            fail++;
            continue;
        }

        close(fd);

        // Сравниваем записанное и прочитанное
        if (write_val == read_val) {
            success++;
        } else {
            std::cerr << "  [FAIL] " << filename
                      << ": wrote 0x" << std::hex << write_val
                      << ", read 0x" << read_val << std::dec << std::endl;
            fail++;
        }
    }

    std::cout << "Результат: " << success << " OK, " << fail << " FAIL"
              << " (всего " << files.size() << ")" << std::endl;
}

// IOCTL: обнуление всех файлов
void cmd_zero_all()
{
    int fd = open_any_file();
    if (fd < 0) return;

    if (ioctl(fd, CWFS_IOC_ZERO_ALL) < 0)
        std::cerr << "IOCTL ZERO_ALL ошибка: " << strerror(errno) << std::endl;
    else
        std::cout << "Все файлы обнулены" << std::endl;

    close(fd);
}

// IOCTL: стирание ФС
void cmd_erase_fs()
{
    std::cout << "ВНИМАНИЕ: Это уничтожит файловую систему!" << std::endl;
    std::cout << "Подтвердите (yes/no): ";
    std::string confirm;
    std::getline(std::cin, confirm);

    if (confirm != "yes") {
        std::cout << "Отменено" << std::endl;
        return;
    }

    int fd = open_any_file();
    if (fd < 0) return;

    if (ioctl(fd, CWFS_IOC_ERASE_FS) < 0)
        std::cerr << "IOCTL ERASE_FS ошибка: " << strerror(errno) << std::endl;
    else
        std::cout << "ФС стёрта. Размонтируйте для завершения." << std::endl;

    close(fd);
}

// IOCTL: получение хешей всех файлов
void cmd_get_meta()
{
    int fd = open_any_file();
    if (fd < 0) return;

    // Буфер: [count] + [hash] * count
    const size_t max_files = 65536;
    size_t buf_size = (1 + max_files) * sizeof(uint32_t);
    uint32_t *buffer = (uint32_t *)malloc(buf_size);
    if (!buffer) {
        std::cerr << "Ошибка выделения памяти" << std::endl;
        close(fd);
        return;
    }
    memset(buffer, 0, buf_size);

    if (ioctl(fd, CWFS_IOC_GET_META, buffer) < 0) {
        std::cerr << "IOCTL GET_META ошибка: " << strerror(errno) << std::endl;
        free(buffer);
        close(fd);
        return;
    }

    uint32_t count = buffer[0];
    std::cout << "Метаинформация (" << count << " файлов):" << std::endl;
    std::cout << "  Файл            | CRC32" << std::endl;
    std::cout << "  ----------------+------------" << std::endl;

    for (uint32_t i = 0; i < count && i < max_files; i++)
        printf("  file_%-10u | 0x%08X\n", i, buffer[i + 1]);

    free(buffer);
    close(fd);
}

// IOCTL: маппинг секторов файла
void cmd_get_mapping(unsigned int file_index)
{
    int fd = open_any_file();
    if (fd < 0) return;

    struct cwfs_mapping_request req;
    memset(&req, 0, sizeof(req));
    req.file_index = file_index;

    if (ioctl(fd, CWFS_IOC_GET_MAPPING, &req) < 0) {
        std::cerr << "IOCTL GET_MAPPING ошибка: " << strerror(errno) << std::endl;
        close(fd);
        return;
    }

    std::cout << "Маппинг file_" << file_index << ":" << std::endl;
    std::cout << "  Начальный сектор: " << req.start_sector << std::endl;
    std::cout << "  Количество секторов: " << req.sector_count << std::endl;
    std::cout << "  Диапазон: [" << req.start_sector << " .. "
              << (req.start_sector + req.sector_count - 1) << "]" << std::endl;
    std::cout << "  Размер: " << (req.sector_count * 512) << " байт" << std::endl;

    close(fd);
}

// Список файлов
void cmd_list()
{
    std::vector<std::string> files = get_file_list();
    std::cout << "Файлы (" << files.size() << "):" << std::endl;
    for (const auto &f : files)
        std::cout << "  " << f << std::endl;
}

// Справка
void cmd_help()
{
    std::cout << "Команды:" << std::endl;
    std::cout << "  test           - записать/прочитать случайное число в каждый файл" << std::endl;
    std::cout << "  zero_all       - обнулить все файлы (IOCTL)" << std::endl;
    std::cout << "  erase_fs       - стереть ФС (IOCTL)" << std::endl;
    std::cout << "  get_meta       - хеши всех файлов (IOCTL)" << std::endl;
    std::cout << "  get_mapping N  - маппинг секторов файла N (IOCTL)" << std::endl;
    std::cout << "  list           - показать список файлов" << std::endl;
    std::cout << "  help           - справка" << std::endl;
    std::cout << "  quit           - выход" << std::endl;
}

// Точка входа
int main(int argc, char *argv[])
{
    if (argc > 1)
        mount_path = argv[1];

    std::cout << "=== curse_work FS - тестовая утилита ===" << std::endl;
    std::cout << "Путь: " << mount_path << std::endl;

    // Проверяем доступность
    DIR *dir = opendir(mount_path.c_str());
    if (!dir) {
        std::cerr << "ОШИБКА: " << mount_path << " недоступен: " << strerror(errno) << std::endl;
        return 1;
    }
    closedir(dir);

    std::vector<std::string> files = get_file_list();
    std::cout << "Найдено файлов: " << files.size() << std::endl;
    std::cout << "Введите 'help' для справки" << std::endl << std::endl;

    // Главный цикл CLI
    std::string line;
    while (true) {
        std::cout << "curse_work> ";
        std::cout.flush();

        if (!std::getline(std::cin, line))
            break;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd.empty()) continue;

        if (cmd == "quit" || cmd == "exit" || cmd == "q") {
            break;
        } else if (cmd == "help" || cmd == "h") {
            cmd_help();
        } else if (cmd == "test") {
            cmd_test();
        } else if (cmd == "zero_all") {
            cmd_zero_all();
        } else if (cmd == "erase_fs") {
            cmd_erase_fs();
        } else if (cmd == "get_meta") {
            cmd_get_meta();
        } else if (cmd == "get_mapping") {
            unsigned int idx;
            if (!(iss >> idx))
                std::cerr << "Использование: get_mapping <номер_файла>" << std::endl;
            else
                cmd_get_mapping(idx);
        } else if (cmd == "list") {
            cmd_list();
        } else {
            std::cerr << "Неизвестная команда: " << cmd << " (введите 'help')" << std::endl;
        }
        std::cout << std::endl;
    }

    std::cout << "Выход." << std::endl;
    return 0;
}
