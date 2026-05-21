#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/crc32.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/blkdev.h>
#include <linux/stat.h>

#include "curse_work.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Custom filesystem module with dual superblock (curse_work)");
MODULE_VERSION("1.0");

// Параметры модуля, задаются при загрузке через insmod
static char *disk_name = "sdb";
module_param(disk_name, charp, 0444);
MODULE_PARM_DESC(disk_name, "Имя блочного устройства для ФС");

static unsigned int sb1_offset = 0;
module_param(sb1_offset, uint, 0444);
MODULE_PARM_DESC(sb1_offset, "Смещение первой копии суперблока (в секторах)");

static unsigned int sb2_offset = 1;
module_param(sb2_offset, uint, 0444);
MODULE_PARM_DESC(sb2_offset, "Смещение второй копии суперблока (в секторах)");

static unsigned int max_name_len = 64;
module_param(max_name_len, uint, 0444);
MODULE_PARM_DESC(max_name_len, "Максимальная длина имени файла");

static unsigned int max_file_sectors = 4;
module_param(max_file_sectors, uint, 0444);
MODULE_PARM_DESC(max_file_sectors, "Максимальный размер файла в секторах (M)");

// Структура информации о суперблоке в памяти (хранится в sb->s_fs_info)
struct cwfs_sb_info {
    uint32_t file_count;         // Количество файлов
    uint32_t file_size_sectors;  // Секторов на файл
    uint32_t max_filename_len;   // Макс. длина имени
    uint32_t sb1_sector;         // Сектор суперблока 1
    uint32_t sb2_sector;         // Сектор суперблока 2
    uint32_t data_start_sector;  // Начало данных
    uint32_t total_sectors;      // Всего секторов
};

// Предварительные объявления
static const struct super_operations cwfs_sb_ops;
static const struct inode_operations cwfs_dir_inode_ops;
static const struct file_operations cwfs_dir_ops;
static const struct inode_operations cwfs_file_inode_ops;
static const struct file_operations cwfs_file_ops;
static const struct fs_context_operations cwfs_context_ops;

// Вычисление CRC32 суперблока (хешируем всё кроме поля checksum)
static uint32_t cwfs_compute_sb_checksum(struct cwfs_sb_disk *dsb)
{
    size_t len = offsetof(struct cwfs_sb_disk, checksum);
    return crc32(0, (const unsigned char *)dsb, len);
}

// Проверка целостности суперблока: магическое число + CRC32
// Возвращает 0 если валиден, -EINVAL если нет
static int cwfs_verify_sb(struct cwfs_sb_disk *dsb)
{
    uint32_t computed;

    // Проверяем магическое число
    if (le32_to_cpu(dsb->magic) != CWFS_MAGIC) {
        pr_debug("curse_work: неверное магическое число: 0x%08x\n",
                 le32_to_cpu(dsb->magic));
        return -EINVAL;
    }

    // Проверяем контрольную сумму
    computed = cwfs_compute_sb_checksum(dsb);
    if (computed != le32_to_cpu(dsb->checksum)) {
        pr_warn("curse_work: несовпадение CRC: вычислено=0x%08x, сохранено=0x%08x\n",
                computed, le32_to_cpu(dsb->checksum));
        return -EINVAL;
    }

    return 0;
}

// Запись суперблока на диск (обе копии)
static int cwfs_write_sb(struct super_block *sb)
{
    struct cwfs_sb_info *sbi = sb->s_fs_info;
    struct cwfs_sb_disk dsb;
    struct buffer_head *bh;

    // Заполняем структуру суперблока
    memset(&dsb, 0, sizeof(dsb));
    dsb.magic = cpu_to_le32(CWFS_MAGIC);
    dsb.version = cpu_to_le32(CWFS_VERSION);
    dsb.sector_size = cpu_to_le32(CWFS_SECTOR_SIZE);
    dsb.total_sectors = cpu_to_le32(sbi->total_sectors);
    dsb.file_count = cpu_to_le32(sbi->file_count);
    dsb.file_size_sectors = cpu_to_le32(sbi->file_size_sectors);
    dsb.max_filename_len = cpu_to_le32(sbi->max_filename_len);
    dsb.sb1_sector = cpu_to_le32(sbi->sb1_sector);
    dsb.sb2_sector = cpu_to_le32(sbi->sb2_sector);
    dsb.data_start_sector = cpu_to_le32(sbi->data_start_sector);
    // Вычисляем CRC32 после заполнения всех полей
    dsb.checksum = cpu_to_le32(cwfs_compute_sb_checksum(&dsb));

    // Записываем первую копию
    bh = sb_bread(sb, sbi->sb1_sector);
    if (!bh) {
        pr_err("curse_work: не удалось прочитать сектор %u для SB1\n", sbi->sb1_sector);
        return -EIO;
    }
    memcpy(bh->b_data, &dsb, sizeof(dsb));
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    // Записываем вторую копию
    bh = sb_bread(sb, sbi->sb2_sector);
    if (!bh) {
        pr_err("curse_work: не удалось прочитать сектор %u для SB2\n", sbi->sb2_sector);
        return -EIO;
    }
    memcpy(bh->b_data, &dsb, sizeof(dsb));
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    pr_info("curse_work: суперблок записан (sb1=%u, sb2=%u, files=%u, crc=0x%08x)\n",
            sbi->sb1_sector, sbi->sb2_sector, sbi->file_count,
            le32_to_cpu(dsb.checksum));
    return 0;
}

// Получение или создание inode
// ino==1: корневой каталог, ino>=2: файл (индекс = ino - 2)
static struct inode *cwfs_get_inode(struct super_block *sb, unsigned long ino)
{
    struct inode *inode;
    struct cwfs_sb_info *sbi = sb->s_fs_info;

    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    // Если inode уже в кеше — просто возвращаем
    if (!(inode->i_state & I_NEW))
        return inode;

    // Инициализация временных меток
    simple_inode_init_ts(inode);

    if (ino == CWFS_ROOT_INO) {
        // Корневой каталог
        inode->i_mode = S_IFDIR | 0755;
        inode->i_op = &cwfs_dir_inode_ops;
        inode->i_fop = &cwfs_dir_ops;
        set_nlink(inode, 2);
        inode->i_size = 0;
    } else {
        // Обычный файл
        inode->i_mode = S_IFREG | 0666;
        inode->i_op = &cwfs_file_inode_ops;
        inode->i_fop = &cwfs_file_ops;
        set_nlink(inode, 1);
        // Размер файла = M секторов * 512 байт
        inode->i_size = (loff_t)sbi->file_size_sectors * CWFS_SECTOR_SIZE;
    }

    unlock_new_inode(inode);
    return inode;
}

// Поиск файла по имени в корневом каталоге
// Ожидаемый формат: "file_<индекс>"
static struct dentry *cwfs_lookup(struct inode *dir, struct dentry *dentry,
                                   unsigned int flags)
{
    struct cwfs_sb_info *sbi = dir->i_sb->s_fs_info;
    const char *name = dentry->d_name.name;
    unsigned int file_idx;
    struct inode *inode;

    // Проверяем префикс
    if (strncmp(name, "file_", 5) != 0)
        return ERR_PTR(-ENOENT);

    // Парсим индекс
    if (kstrtouint(name + 5, 10, &file_idx) != 0)
        return ERR_PTR(-ENOENT);

    // Проверяем что индекс в допустимом диапазоне
    if (file_idx >= sbi->file_count)
        return ERR_PTR(-ENOENT);

    // Получаем inode и привязываем к dentry
    inode = cwfs_get_inode(dir->i_sb, file_idx + CWFS_FIRST_FILE_INO);
    if (IS_ERR(inode))
        return ERR_CAST(inode);

    d_add(dentry, inode);
    return NULL;
}

// Перечисление файлов в каталоге (вызывается при ls)
static int cwfs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct inode *inode = file_inode(file);
    struct cwfs_sb_info *sbi = inode->i_sb->s_fs_info;
    char name_buf[128];
    unsigned int idx;

    // Стандартные записи "." и ".."
    if (!dir_emit_dots(file, ctx))
        return 0;

    // Перечисляем файлы начиная с текущей позиции
    idx = ctx->pos - 2;
    while (idx < sbi->file_count) {
        int name_len = snprintf(name_buf, sizeof(name_buf), "file_%u", idx);

        if (!dir_emit(ctx, name_buf, name_len,
                      idx + CWFS_FIRST_FILE_INO, DT_REG))
            return 0;

        ctx->pos++;
        idx++;
    }

    return 0;
}

// Чтение данных файла
// Файл file_idx занимает секторы [data_start + idx*M .. data_start + (idx+1)*M - 1]
static ssize_t cwfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct inode *inode = file_inode(iocb->ki_filp);
    struct super_block *sb = inode->i_sb;
    struct cwfs_sb_info *sbi = sb->s_fs_info;
    unsigned long file_idx = inode->i_ino - CWFS_FIRST_FILE_INO;
    loff_t file_size = (loff_t)sbi->file_size_sectors * CWFS_SECTOR_SIZE;
    sector_t start_sector = sbi->data_start_sector + file_idx * sbi->file_size_sectors;
    loff_t pos = iocb->ki_pos;
    size_t count = iov_iter_count(to);
    ssize_t ret = 0;

    // Проверка границ файла
    if (pos >= file_size)
        return 0;
    if (pos + count > file_size)
        count = file_size - pos;
    if (count == 0)
        return 0;

    // Читаем посекторно
    while (count > 0) {
        sector_t cur_sector = start_sector + (pos / CWFS_SECTOR_SIZE);
        unsigned int offset_in_sector = pos % CWFS_SECTOR_SIZE;
        unsigned int to_read = min_t(unsigned int, CWFS_SECTOR_SIZE - offset_in_sector, count);
        struct buffer_head *bh;
        size_t copied;

        // Читаем сектор с устройства
        bh = sb_bread(sb, cur_sector);
        if (!bh) {
            pr_err("curse_work: ошибка чтения сектора %llu\n", (unsigned long long)cur_sector);
            return ret ? ret : -EIO;
        }

        // Копируем в пользовательский буфер
        copied = copy_to_iter(bh->b_data + offset_in_sector, to_read, to);
        brelse(bh);

        if (copied != to_read)
            return ret ? ret : -EFAULT;

        pos += to_read;
        count -= to_read;
        ret += to_read;
    }

    iocb->ki_pos = pos;
    return ret;
}

// Запись данных в файл (размер фиксирован, за границу нельзя)
static ssize_t cwfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct inode *inode = file_inode(iocb->ki_filp);
    struct super_block *sb = inode->i_sb;
    struct cwfs_sb_info *sbi = sb->s_fs_info;
    unsigned long file_idx = inode->i_ino - CWFS_FIRST_FILE_INO;
    loff_t file_size = (loff_t)sbi->file_size_sectors * CWFS_SECTOR_SIZE;
    sector_t start_sector = sbi->data_start_sector + file_idx * sbi->file_size_sectors;
    loff_t pos = iocb->ki_pos;
    size_t count = iov_iter_count(from);
    ssize_t ret = 0;

    // Нельзя писать за границей файла
    if (pos >= file_size)
        return -ENOSPC;
    if (pos + count > file_size)
        count = file_size - pos;
    if (count == 0)
        return 0;

    // Записываем посекторно
    while (count > 0) {
        sector_t cur_sector = start_sector + (pos / CWFS_SECTOR_SIZE);
        unsigned int offset_in_sector = pos % CWFS_SECTOR_SIZE;
        unsigned int to_write = min_t(unsigned int, CWFS_SECTOR_SIZE - offset_in_sector, count);
        struct buffer_head *bh;
        size_t copied;

        // Читаем сектор (может содержать другие данные)
        bh = sb_bread(sb, cur_sector);
        if (!bh) {
            pr_err("curse_work: ошибка чтения сектора %llu для записи\n", (unsigned long long)cur_sector);
            return ret ? ret : -EIO;
        }

        // Копируем из пользовательского пространства
        copied = copy_from_iter(bh->b_data + offset_in_sector, to_write, from);
        if (copied != to_write) {
            brelse(bh);
            return ret ? ret : -EFAULT;
        }

        // Помечаем грязным и при необходимости синхронизируем
        mark_buffer_dirty(bh);
        if (iocb->ki_flags & IOCB_DSYNC)
            sync_dirty_buffer(bh);
        brelse(bh);

        pos += to_write;
        count -= to_write;
        ret += to_write;
    }

    iocb->ki_pos = pos;
    return ret;
}

// Вычисление CRC32 содержимого файла (для IOCTL GET_META)
static uint32_t cwfs_compute_file_hash(struct super_block *sb, unsigned int file_idx)
{
    struct cwfs_sb_info *sbi = sb->s_fs_info;
    sector_t start = sbi->data_start_sector + (sector_t)file_idx * sbi->file_size_sectors;
    uint32_t crc = 0;
    unsigned int i;

    for (i = 0; i < sbi->file_size_sectors; i++) {
        struct buffer_head *bh = sb_bread(sb, start + i);
        if (!bh)
            return 0;
        crc = crc32(crc, bh->b_data, CWFS_SECTOR_SIZE);
        brelse(bh);
    }

    return crc;
}

// Обработчик IOCTL (4 команды)
static long cwfs_file_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct inode *inode = file_inode(filp);
    struct super_block *sb = inode->i_sb;
    struct cwfs_sb_info *sbi = sb->s_fs_info;
    unsigned int i, j;

    switch (cmd) {
    case CWFS_IOC_ZERO_ALL: {
        // Обнуляем содержимое всех файлов
        pr_info("curse_work: IOCTL ZERO_ALL - обнуление %u файлов\n", sbi->file_count);

        for (i = 0; i < sbi->file_count; i++) {
            sector_t start = sbi->data_start_sector + (sector_t)i * sbi->file_size_sectors;
            for (j = 0; j < sbi->file_size_sectors; j++) {
                struct buffer_head *bh = sb_bread(sb, start + j);
                if (!bh)
                    return -EIO;
                memset(bh->b_data, 0, CWFS_SECTOR_SIZE);
                mark_buffer_dirty(bh);
                sync_dirty_buffer(bh);
                brelse(bh);
            }
        }
        pr_info("curse_work: все файлы обнулены\n");
        break;
    }

    case CWFS_IOC_ERASE_FS: {
        // Стираем обе копии суперблока (инвалидация ФС)
        struct buffer_head *bh;
        pr_info("curse_work: IOCTL ERASE_FS - стирание суперблоков\n");

        bh = sb_bread(sb, sbi->sb1_sector);
        if (!bh)
            return -EIO;
        memset(bh->b_data, 0, CWFS_SECTOR_SIZE);
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);

        bh = sb_bread(sb, sbi->sb2_sector);
        if (!bh)
            return -EIO;
        memset(bh->b_data, 0, CWFS_SECTOR_SIZE);
        mark_buffer_dirty(bh);
        sync_dirty_buffer(bh);
        brelse(bh);

        pr_info("curse_work: ФС стёрта\n");
        break;
    }

    case CWFS_IOC_GET_META: {
        // Возвращаем CRC32 каждого файла: [count][hash0][hash1]...
        uint32_t __user *ubuf = (uint32_t __user *)arg;
        uint32_t hash;

        pr_info("curse_work: IOCTL GET_META - %u файлов\n", sbi->file_count);

        // Первый элемент — количество файлов
        if (put_user(sbi->file_count, ubuf))
            return -EFAULT;
        ubuf++;

        // Далее хеш каждого файла
        for (i = 0; i < sbi->file_count; i++) {
            hash = cwfs_compute_file_hash(sb, i);
            if (put_user(hash, ubuf + i))
                return -EFAULT;
        }
        break;
    }

    case CWFS_IOC_GET_MAPPING: {
        // Возвращаем начальный сектор и количество секторов для файла
        struct cwfs_mapping_request req;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;

        if (req.file_index >= sbi->file_count)
            return -EINVAL;

        req.sector_count = sbi->file_size_sectors;
        req.start_sector = sbi->data_start_sector +
                           (uint64_t)req.file_index * sbi->file_size_sectors;

        if (copy_to_user((void __user *)arg, &req, sizeof(req)))
            return -EFAULT;

        pr_info("curse_work: IOCTL GET_MAPPING file_%u -> sector %llu, count %u\n",
                req.file_index, req.start_sector, req.sector_count);
        break;
    }

    default:
        return -ENOTTY;
    }

    return 0;
}

// Файловые операции для обычных файлов
static const struct file_operations cwfs_file_ops = {
    .owner          = THIS_MODULE,
    .read_iter      = cwfs_read_iter,
    .write_iter     = cwfs_write_iter,
    .unlocked_ioctl = cwfs_file_ioctl,
    .llseek         = default_llseek,
};

// Inode-операции для файлов
static const struct inode_operations cwfs_file_inode_ops = {
    .setattr = simple_setattr,
    .getattr = simple_getattr,
};

// Файловые операции для корневого каталога
static const struct file_operations cwfs_dir_ops = {
    .owner          = THIS_MODULE,
    .iterate_shared = cwfs_iterate_shared,
    .read           = generic_read_dir,
    .llseek         = default_llseek,
};

// Inode-операции для каталога
static const struct inode_operations cwfs_dir_inode_ops = {
    .lookup = cwfs_lookup,
};

// Освобождение ресурсов при размонтировании
static void cwfs_put_super(struct super_block *sb)
{
    struct cwfs_sb_info *sbi = sb->s_fs_info;
    pr_info("curse_work: размонтирование ФС\n");
    kfree(sbi);
    sb->s_fs_info = NULL;
}

// Операции суперблока
static const struct super_operations cwfs_sb_ops = {
    .put_super = cwfs_put_super,
    .statfs    = simple_statfs,
};

// Инициализация суперблока при монтировании
// Пытаемся прочитать SB1, затем SB2, если оба невалидны — форматируем
static int cwfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct cwfs_sb_info *sbi;
    struct cwfs_sb_disk dsb;
    struct buffer_head *bh;
    struct inode *root_inode;
    int sb_found = 0;
    sector_t total_sectors;

    // Устанавливаем размер блока = размеру сектора
    if (!sb_set_blocksize(sb, CWFS_SECTOR_SIZE)) {
        pr_err("curse_work: не удалось установить размер блока %d\n", CWFS_SECTOR_SIZE);
        return -EINVAL;
    }

    // Узнаём размер устройства
    total_sectors = (sector_t)(bdev_nr_bytes(sb->s_bdev) / CWFS_SECTOR_SIZE);
    pr_info("curse_work: устройство: %llu секторов\n", (unsigned long long)total_sectors);

    // Выделяем память под метаданные ФС
    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sb->s_fs_info = sbi;
    sb->s_magic = CWFS_MAGIC;
    sb->s_op = &cwfs_sb_ops;
    sb->s_maxbytes = MAX_LFS_FILESIZE;

    // Пробуем прочитать первую копию суперблока
    if (sb1_offset < total_sectors) {
        bh = sb_bread(sb, sb1_offset);
        if (bh) {
            memcpy(&dsb, bh->b_data, sizeof(dsb));
            brelse(bh);
            if (cwfs_verify_sb(&dsb) == 0) {
                pr_info("curse_work: SB1 валиден (сектор %u)\n", sb1_offset);
                sb_found = 1;
            } else {
                pr_info("curse_work: SB1 невалиден (сектор %u)\n", sb1_offset);
            }
        }
    }

    // Если первая невалидна — пробуем вторую
    if (!sb_found && sb2_offset < total_sectors) {
        bh = sb_bread(sb, sb2_offset);
        if (bh) {
            memcpy(&dsb, bh->b_data, sizeof(dsb));
            brelse(bh);
            if (cwfs_verify_sb(&dsb) == 0) {
                pr_info("curse_work: SB2 валиден (сектор %u)\n", sb2_offset);
                sb_found = 1;
            } else {
                pr_info("curse_work: SB2 невалиден (сектор %u)\n", sb2_offset);
            }
        }
    }

    if (sb_found) {
        // Загружаем параметры из существующего суперблока
        sbi->file_count = le32_to_cpu(dsb.file_count);
        sbi->file_size_sectors = le32_to_cpu(dsb.file_size_sectors);
        sbi->max_filename_len = le32_to_cpu(dsb.max_filename_len);
        sbi->sb1_sector = le32_to_cpu(dsb.sb1_sector);
        sbi->sb2_sector = le32_to_cpu(dsb.sb2_sector);
        sbi->data_start_sector = le32_to_cpu(dsb.data_start_sector);
        sbi->total_sectors = le32_to_cpu(dsb.total_sectors);
        pr_info("curse_work: загружена ФС: %u файлов по %u секторов\n",
                sbi->file_count, sbi->file_size_sectors);
    } else {
        // Форматирование: создаём новую ФС
        uint32_t data_start;

        pr_info("curse_work: суперблок не найден — форматирование\n");

        // Валидация параметров
        if (sb1_offset >= total_sectors || sb2_offset >= total_sectors) {
            pr_err("curse_work: смещения SB за границами диска\n");
            kfree(sbi);
            return -EINVAL;
        }
        if (sb1_offset == sb2_offset) {
            pr_err("curse_work: смещения SB должны различаться\n");
            kfree(sbi);
            return -EINVAL;
        }
        if (max_file_sectors == 0) {
            pr_err("curse_work: размер файла должен быть > 0\n");
            kfree(sbi);
            return -EINVAL;
        }

        // Данные начинаются после последнего суперблока
        data_start = max(sb1_offset, sb2_offset) + 1;

        sbi->sb1_sector = sb1_offset;
        sbi->sb2_sector = sb2_offset;
        sbi->data_start_sector = data_start;
        sbi->file_size_sectors = max_file_sectors;
        sbi->max_filename_len = max_name_len;
        sbi->total_sectors = (uint32_t)total_sectors;

        // Количество файлов = (оставшиеся секторы) / M
        sbi->file_count = (total_sectors - data_start) / max_file_sectors;

        if (sbi->file_count == 0) {
            pr_err("curse_work: диск слишком мал\n");
            kfree(sbi);
            return -EINVAL;
        }

        pr_info("curse_work: создано %u файлов по %u секторов, данные с сектора %u\n",
                sbi->file_count, sbi->file_size_sectors, sbi->data_start_sector);

        // Записываем обе копии суперблока на диск
        if (cwfs_write_sb(sb) != 0) {
            kfree(sbi);
            return -EIO;
        }
    }

    // Создаём корневой inode
    root_inode = cwfs_get_inode(sb, CWFS_ROOT_INO);
    if (IS_ERR(root_inode)) {
        pr_err("curse_work: ошибка создания корневого inode\n");
        kfree(sbi);
        return PTR_ERR(root_inode);
    }

    // Создаём корневой dentry
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        pr_err("curse_work: ошибка создания корневого dentry\n");
        kfree(sbi);
        return -ENOMEM;
    }

    pr_info("curse_work: ФС смонтирована успешно\n");
    return 0;
}

// fs_context API — новый способ монтирования (ядро 6.x)
static int cwfs_get_tree(struct fs_context *fc)
{
    return get_tree_bdev(fc, cwfs_fill_super);
}

static const struct fs_context_operations cwfs_context_ops = {
    .get_tree = cwfs_get_tree,
};

static int cwfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &cwfs_context_ops;
    return 0;
}

// Регистрация типа ФС в VFS
static struct file_system_type cwfs_fs_type = {
    .owner            = THIS_MODULE,
    .name             = "curse_work_fs",
    .init_fs_context  = cwfs_init_fs_context,
    .kill_sb          = kill_block_super,
    .fs_flags         = FS_REQUIRES_DEV,
};

// Инициализация модуля (insmod)
static int __init cwfs_init(void)
{
    int ret;

    pr_info("curse_work: загрузка модуля (disk=%s, sb1=%u, sb2=%u, max_name=%u, M=%u)\n",
            disk_name, sb1_offset, sb2_offset, max_name_len, max_file_sectors);

    // Регистрируем ФС — после этого доступна mount -t curse_work_fs
    ret = register_filesystem(&cwfs_fs_type);
    if (ret) {
        pr_err("curse_work: ошибка регистрации: %d\n", ret);
        return ret;
    }

    pr_info("curse_work: ФС зарегистрирована как 'curse_work_fs'\n");
    return 0;
}

// Выгрузка модуля (rmmod)
static void __exit cwfs_exit(void)
{
    unregister_filesystem(&cwfs_fs_type);
    pr_info("curse_work: модуль выгружен\n");
}

module_init(cwfs_init);
module_exit(cwfs_exit);
