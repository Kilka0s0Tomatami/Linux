#ifndef CURSE_WORK_H
#define CURSE_WORK_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

// Магическое число для идентификации нашей ФС
#define CWFS_MAGIC          0x43574653  // "CWFS" в hex
#define CWFS_VERSION        1
#define CWFS_SECTOR_SIZE    512
#define CWFS_ROOT_INO       1           // Номер inode корневого каталога
#define CWFS_FIRST_FILE_INO 2           // Первый inode файлов

// Структура суперблока на диске (ровно 512 байт = 1 сектор)
// Две копии хранятся для отказоустойчивости
struct cwfs_sb_disk {
    uint32_t magic;              // Магическое число CWFS_MAGIC
    uint32_t version;            // Версия ФС
    uint32_t sector_size;        // Размер сектора (512)
    uint32_t total_sectors;      // Общее количество секторов на устройстве
    uint32_t file_count;         // Количество файлов в ФС
    uint32_t file_size_sectors;  // Размер каждого файла в секторах (M)
    uint32_t max_filename_len;   // Максимальная длина имени файла
    uint32_t sb1_sector;         // Сектор первой копии суперблока
    uint32_t sb2_sector;         // Сектор второй копии суперблока
    uint32_t data_start_sector;  // Первый сектор области данных
    uint32_t checksum;           // CRC32 всех полей кроме этого
    uint8_t  padding[512 - 44]; // Дополнение до размера сектора
};

// Определения IOCTL
#define CWFS_IOC_MAGIC 'C'

// Обнуление данных во всех файлах
#define CWFS_IOC_ZERO_ALL     _IO(CWFS_IOC_MAGIC, 0)

// Стереть ФС (инвалидация суперблоков)
#define CWFS_IOC_ERASE_FS     _IO(CWFS_IOC_MAGIC, 1)

// Получить хеши всех файлов, формат: [uint32_t count][uint32_t hash_0]...
#define CWFS_IOC_GET_META     _IOR(CWFS_IOC_MAGIC, 2, uint64_t)

// Получить маппинг секторов для заданного файла
#define CWFS_IOC_GET_MAPPING  _IOWR(CWFS_IOC_MAGIC, 3, struct cwfs_mapping_request)

// Структура запроса маппинга секторов
struct cwfs_mapping_request {
    uint32_t file_index;     // вход: индекс файла
    uint32_t sector_count;   // выход: количество секторов
    uint64_t start_sector;   // выход: начальный сектор на диске
};

#endif // CURSE_WORK_H
