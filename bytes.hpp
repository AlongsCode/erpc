#ifndef KRNLN_BYTES_HPP
#define KRNLN_BYTES_HPP
#include "utils.hpp"

#include <expected>
#include <string>
#include <atomic>
#include <bit>
#include <stdexcept>
#include <vector>
#include <limits>
#include <fstream>
#include <random>
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <span>

namespace erpc_imp {
    namespace _conversion {

        /**
         * @brief 强制字节序转换（如将主机序转为大端）
         * @tparam T 算术类型
         * @param val 输入值
         * @return 字节序交换后的值
         */
        template <typename T>
        [[nodiscard]] inline T swap_endian(T val) {
            static_assert(std::is_arithmetic_v<T>, "T must be an arithmetic type");
            T ret;
            uint8_t* src = reinterpret_cast<uint8_t*>(&val);
            uint8_t* dst = reinterpret_cast<uint8_t*>(&ret);
            for (size_t i = 0; i < sizeof(T); ++i) {
                dst[i] = src[sizeof(T) - 1 - i];
            }
            return ret;
        }

        /**
         * @brief 转换为十进制文本格式
         * @tparam CharType 字符类型
         * @param data 数据指针
         * @param size 数据大小
         * @return 格式如"{1,2,3}"的字符串
         */
        template<typename CharType = char>
        [[nodiscard]] inline std::basic_string<CharType> to_decimal(const uint8_t* data, size_t size) {
            if (size == 0) return {};
            std::basic_string<CharType> result;
            result.reserve(size * 4 + 3);
            result.push_back('{');
            for (size_t i = 0; i < size; ++i) {
                uint8_t byte = data[i];
                if (byte >= 100) {
                    result.push_back(static_cast<CharType>('0' + byte / 100));
                    result.push_back(static_cast<CharType>('0' + (byte % 100) / 10));
                }
                else if (byte >= 10) {
                    result.push_back(static_cast<CharType>('0' + byte / 10));
                }
                result.push_back(static_cast<CharType>('0' + byte % 10));
                if (i != size - 1) result.push_back(',');
            }
            result.push_back('}');
            return result;
        }

        /**
         * @brief 转换为十六进制文本
         * @tparam CharType 字符类型
         * @param data 数据指针
         * @param size 数据大小
         * @param use_lowercase 是否使用小写字母
         * @return 十六进制字符串
         */
        template<typename CharType = char>
        [[nodiscard]] inline std::basic_string<CharType> to_hex(const uint8_t* data, size_t size, bool use_lowercase = true) {
            if (size == 0) return {};
            static constexpr CharType hex_digits_upper[16] = {
                '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'
            };
            static constexpr CharType hex_digits_lower[16] = {
                '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
            };
            const auto* digits = use_lowercase ? hex_digits_lower : hex_digits_upper;
            std::basic_string<CharType> result(size * 2, CharType{});
            for (size_t i = 0; i < size; ++i) {
                uint8_t byte = data[i];
                result[i * 2] = digits[byte >> 4];
                result[i * 2 + 1] = digits[byte & 0x0F];
            }
            return result;
        }

        /**
         * @brief Base64编码
         * @tparam CharType 字符类型
         * @param data 数据指针
         * @param size 数据大小
         * @param alphabet Base64字母表
         * @return Base64编码字符串
         */
        template<typename CharType = char>
        [[nodiscard]] inline std::basic_string<CharType> to_base64(
            const uint8_t* data, size_t size,
            const std::basic_string<CharType>& alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") {
            if (size == 0) return {};
            std::basic_string<CharType> result;
            result.reserve((size + 2) / 3 * 4);
            for (size_t i = 0; i < size; i += 3) {
                uint32_t triple = 0;
                size_t bytes_in_triple = std::min(size - i, size_t(3));
                for (size_t j = 0; j < bytes_in_triple; ++j) {
                    triple |= static_cast<uint32_t>(data[i + j]) << (16 - j * 8);
                }
                for (size_t j = 0; j < 4; ++j) {
                    if (j * 6 < bytes_in_triple * 8) {
                        uint8_t index = (triple >> (18 - j * 6)) & 0x3F;
                        result.push_back(alphabet[index]);
                    }
                    else {
                        result.push_back('=');
                    }
                }
            }
            return result;
        }

        /**
         * @brief 转换为字符串（按原始字节解释）
         * @tparam CharType 字符类型
         * @param data 数据指针
         * @param size 数据大小
         * @return 字符串表示
         */
        template<typename CharType = char>
        [[nodiscard]] inline std::basic_string<CharType> to_string(const uint8_t* data, size_t size) {
            if (size == 0) return {};
            return std::basic_string<CharType>(
                reinterpret_cast<const CharType*>(data),
                size / sizeof(CharType));
        }
    }

    class bytes {
    public:
        /**
         * @class view
         * @brief bytes 的只读视图类
         *
         * 提供对 bytes 对象的只读访问，支持查找等操作。
         */
        class view {
            const uint8_t* _ptr{nullptr};
            size_t _len{0};
        public:
            view() = default;
            // ==================== 构造函数 ====================
            /** @brief 从 bytes 构造视图 */
            view(const bytes& m) : _ptr(m.data()), _len(m.size()) {}
            /** @brief 从指针和长度构造视图 */
            view(const uint8_t* p, size_t l) : _ptr(p), _len(l) {}

            // ==================== 基本属性 ====================
            /** @brief 获取数据指针 */
            [[nodiscard]] const uint8_t* data() const { return _ptr; }
            /** @brief 获取数据大小（字节） */
            [[nodiscard]] size_t size() const { return _len; }
            /** @brief 检查是否为空 */
            [[nodiscard]] bool empty() const { return _len == 0; }

            // ==================== 查找操作 ====================
            /**
             * @brief 正向查找子序列
             * @param pattern 要查找的字节序列
             * @param start_offset 起始搜索位置
             * @return 找到的位置索引，未找到返回 npos
             */
            [[nodiscard]] std::optional<size_t> find(const bytes& pattern, size_t start_offset = 0) const {
                return bytes::internal_find(_ptr, _len, pattern.data(), pattern.size(), start_offset);
            }

            /**
             * @brief 反向查找子序列
             * @param pattern 要查找的字节序列
             * @param start_offset 起始搜索位置
             * @return 找到的位置索引，未找到返回 npos
             */
            [[nodiscard]] std::optional<size_t> rfind(const bytes& pattern, size_t start_offset = bytes::maxbytes_count) const {
                return bytes::internal_reverse_find(_ptr, _len, pattern.data(), pattern.size(), start_offset);
            }

            /** @brief 取左侧子视图 */
            [[nodiscard]] view left(size_t n) const {
                return view(_ptr, std::min(n, _len));
            }

            /** @brief 取中间子视图 */
            [[nodiscard]] view mid(size_t pos, size_t n = bytes::maxbytes_count) const {
                if (pos >= _len) return { nullptr, 0 };
                return view(_ptr + pos, std::min(n, _len - pos));
            }

            /** @brief 取右侧子视图 */
            [[nodiscard]] view right(size_t n) const {
                if (n >= _len) return view(_ptr, _len);
                return view(_ptr + (_len - n), n);
            }

            /** @brief 转换为独立的 bytes 对象 */
            [[nodiscard]] bytes to_bytes() const {
                return bytes(_ptr, _len);
            }

            // ==================== 转换操作 ====================
            /** @brief 转换为十进制文本格式 */
            template<typename CharType = char>
            [[nodiscard]] std::basic_string<CharType> decimal() const {
                return _conversion::to_decimal<CharType>(_ptr, _len);
            }

            /** @brief 转换为十六进制文本 */
            template<typename CharType = char>
            [[nodiscard]] std::basic_string<CharType> hex(bool use_lowercase = true) const {
                return _conversion::to_hex<CharType>(_ptr, _len, use_lowercase);
            }

            /** @brief Base64 编码 */
            template<typename CharType = char>
            [[nodiscard]] std::basic_string<CharType> base64(
                const std::basic_string<CharType>& alphabet =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") const {
                return _conversion::to_base64<CharType>(_ptr, _len, alphabet);
            }

            /** @brief 转换为字符串（按原始字节解释） */
            template<typename CharType = char>
            [[nodiscard]] std::basic_string<CharType> to_string() const {
                return _conversion::to_string<CharType>(_ptr, _len);
            }

            // ==================== 元素访问 ====================
            /**
             * @brief 带边界检查的元素访问（只读），返回 expected
             * @param position 位置索引
             * @return 指定位置的字节，或错误信息
             */
            [[nodiscard]] std::expected<uint8_t, std::string> at(size_t position) const {
                if (position >= _len) {
                    return std::unexpected("view index out of range");
                }
                return _ptr[position];
            }

            /** @brief 下标访问操作符（只读，不检查边界） */
            auto operator[](size_t position) const {
                return _ptr[position];
            }

            /** @brief 获取第一个字节 */
            auto front() const noexcept { return _ptr[0]; }
            /** @brief 获取最后一个字节 */
            auto back() const noexcept { return _ptr[_len - 1]; }
        };

        // ==================== 类型别名 ====================
        using byte_type = uint8_t;   // 唯一的类型别名，可改为 std::byte

        static constexpr bool kIsLittleEndian = (std::endian::native == std::endian::little);
        // 数值类内存布局
        enum class Endianness {
            BigEndian,
            LittleEndian,
            Native = static_cast<int>(std::endian::native == std::endian::little ? 1 : 0)
        };

        // ==================== 构造与析构 ====================
        /** @brief 默认构造空字节集 */
        bytes() noexcept { reset_to_empty(); }

        /** @brief 拷贝构造 */
        bytes(const bytes& source) {
            reset_to_empty();
            switch (source.storage_category()) {
            case StorageCategory::isSmall: copy_small_storage(source); break;
            case StorageCategory::isMedium: copy_medium_storage(source); break;
            case StorageCategory::isLarge: copy_large_storage(source); break;
            }
        }

        /** @brief 移动构造 */
        bytes(bytes&& source) noexcept {
            medium_large = source.medium_large;
            source.reset_to_empty();
        }

        /**
         * @brief 从原始数据构造
         * @param data_pointer 数据指针
         * @param data_size 数据大小（字节）
         */
        bytes(const void* data_pointer, size_t data_size) {
            reset_to_empty();
            if (data_size == 0) return;
            const byte_type* byte_data = reinterpret_cast<const byte_type*>(data_pointer);
            if (data_size <= kMaxSmallSize) {
                initialize_small_storage(byte_data, data_size);
            }
            else if (data_size <= kMaxMediumSize) {
                initialize_medium_storage(byte_data, data_size);
            }
            else {
                initialize_large_storage(byte_data, data_size);
            }
        }

        /** @brief 从迭代器范围构造（迭代器类型为 byte_type*） */
        bytes(const byte_type* begin_iterator, const byte_type* end_iterator)
            : bytes(begin_iterator, static_cast<size_t>(end_iterator - begin_iterator)) {}

        /**
         * @brief 重复单个字节构造
         * @param repeat_count 重复次数
         * @param fill_byte 填充字节值（默认为0）
         */
        bytes(size_t repeat_count, byte_type fill_byte = 0) {
            reset_to_empty();
            if (repeat_count == 0) return;
            auto new_data_ptr = expand_without_initialization(repeat_count);
            std::memset(new_data_ptr, fill_byte, repeat_count);
        }

        /**
         * @brief 重复字节序列构造
         * @param repeat_count 重复次数
         * @param pattern 重复的字节序列
         */
        bytes(size_t repeat_count, const bytes& pattern) {
            reset_to_empty();
            if (repeat_count == 0 || pattern.empty()) return;
            auto new_data_ptr = expand_without_initialization(repeat_count * pattern.size());
            for (size_t i = 0; i < repeat_count; ++i) {
                std::memcpy(new_data_ptr, pattern.data(), pattern.size());
                new_data_ptr += pattern.size();
            }
        }

        /** @brief 初始化列表构造 */
        bytes(std::initializer_list<byte_type> initializer_list) {
            reset_to_empty();
            reserve(initializer_list.size());
            for (auto value : initializer_list) push_back(static_cast<byte_type>(value));
        }

        /** @brief 析构函数 */
        ~bytes() noexcept {
            if (storage_category() != StorageCategory::isSmall) {
                destroy_medium_large_storage();
            }
        }

        // ==================== 赋值操作符 ====================
        /** @brief 拷贝赋值 */
        bytes& operator=(const bytes& source) {
            if (this != &source) assign(source.data(), source.size());
            return *this;
        }

        /** @brief 移动赋值 */
        bytes& operator=(bytes&& source) noexcept {
            if (this != &source) {
                if (storage_category() != StorageCategory::isSmall) destroy_medium_large_storage();
                medium_large = source.medium_large;
                source.reset_to_empty();
            }
            return *this;
        }

        /** @brief 初始化列表赋值 */
        bytes& operator=(std::initializer_list<byte_type> initializer_list) {
            return assign(initializer_list.begin(), initializer_list.size());
        }

        // ==================== 迭代器 ====================
        byte_type* begin() noexcept { return mutable_data(); }
        const byte_type* begin() const noexcept { return data(); }
        const byte_type* cbegin() const noexcept { return begin(); }
        byte_type* end() noexcept { return mutable_data() + size(); }
        const byte_type* end() const noexcept { return data() + size(); }
        const byte_type* cend() const noexcept { return end(); }
        std::reverse_iterator<byte_type*> rbegin() noexcept { return std::reverse_iterator<byte_type*>(end()); }
        std::reverse_iterator<const byte_type*> rbegin() const noexcept { return std::reverse_iterator<const byte_type*>(end()); }
        std::reverse_iterator<const byte_type*> crbegin() const noexcept { return rbegin(); }
        std::reverse_iterator<byte_type*> rend() noexcept { return std::reverse_iterator<byte_type*>(begin()); }
        std::reverse_iterator<const byte_type*> rend() const noexcept { return std::reverse_iterator<const byte_type*>(begin()); }
        std::reverse_iterator<const byte_type*> crend() const noexcept { return rend(); }

        // ==================== 容量操作 ====================
        /** @brief 获取字节序列大小（字节数） */
        [[nodiscard]] size_t size() const noexcept {
            size_t ret = medium_large.current_size;
            if constexpr (kIsLittleEndian) {
                auto maybeSmallSize = size_t(kMaxSmallSize) -
                    size_t(static_cast<uint8_t>(small_data[kMaxSmallSize]));
                // GCC 和 Clang 会生成 CMOV 指令而非分支指令。fix:参考fbstring实现
                ret =
                    (static_cast<ptrdiff_t>(maybeSmallSize) >= 0) ? maybeSmallSize : ret;
            }
            else {
                ret = (storage_category() == StorageCategory::isSmall) ? small_storage_size() : ret;
            }
            return ret;
        }

        /** @brief 检查是否为空 */
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }


        /**
         * @brief 获取当前分配容量
         * @return 当前分配的容量（字节）
         */
        [[nodiscard]] size_t capacity() const {
            switch (storage_category()) {
            case StorageCategory::isSmall:
                return kMaxSmallSize;
            case StorageCategory::isLarge:
                // 共享的大数据没有可用容量
                if (RefCountedBlock::get_reference_count(medium_large.data_ptr) > 1) {
                    return medium_large.current_size;
                }
                break;
            default:
                break;
            }
            return medium_large.actual_capacity();
        }

        /** @brief 预留容量 */
        void reserve(size_t min_capacity) {
            switch (storage_category()) {
            case StorageCategory::isSmall: reserve_small_storage(min_capacity); break;
            case StorageCategory::isMedium: reserve_medium_storage(min_capacity); break;
            case StorageCategory::isLarge: reserve_large_storage(min_capacity); break;
            }
        }

        /**
         * @brief 调整字节序列大小
         * @param new_size 新的大小
         * @param fill_byte 填充字节（默认为0）
         */
        void resize(size_t new_size, byte_type fill_byte = 0) {
            size_t current_size = size();
            if (new_size <= current_size) {
                shrink_by(current_size - new_size);
            }
            else {
                auto growth_amount = new_size - current_size;
                auto new_data_ptr = expand_without_initialization(growth_amount);
                std::memset(new_data_ptr, fill_byte, growth_amount);
            }
        }

        /** @brief 清空字节序列 */
        void clear() noexcept { shrink_by(size()); }

        /** @brief 交换两个字节序列 */
        void swap(bytes& other) noexcept { std::swap(medium_large, other.medium_large); }

        // ==================== 元素访问 ====================
        /** @brief 获取可修改的数据指针 */
        [[nodiscard]] byte_type* mutable_data() {
            switch (storage_category()) {
            case StorageCategory::isSmall: return small_data;
            case StorageCategory::isMedium: return medium_large.data_ptr;
            case StorageCategory::isLarge: return get_mutable_large_data();
            default: throw std::bad_alloc();
            }
        }

        /** @brief 获取只读数据指针 */
        [[nodiscard]] const byte_type* data() const noexcept {
            return (storage_category() == StorageCategory::isSmall) ? small_data : medium_large.data_ptr;
        }

        /** @brief 检查数据是否被共享（仅用于大数据） */
        [[nodiscard]] bool is_shared() const noexcept {
            if (storage_category() != StorageCategory::isLarge) return false;
            return RefCountedBlock::get_reference_count(medium_large.data_ptr) > 1;
        }

        /** @brief 安全擦除字节序列内容（物理填零） */
        void secure_erase() noexcept {
            size_t current_size = size();
            if (current_size == 0) return;
            if (storage_category() == StorageCategory::isSmall) {
                std::memset(small_data, 0, kMaxSmallSize);
            }
            else {
                
                if (is_med() || (is_lrg() && RefCountedBlock::get_reference_count(medium_large.data_ptr) == 1)) {
                    auto size = medium_large.actual_capacity();
                    volatile unsigned char* p = static_cast<volatile unsigned char*>(medium_large.data_ptr);
                    while (size--) *p++ = 0;
                }
            }
            clear();
        }

        /** @brief 带边界检查的访问，返回 expected（按值） */
        [[nodiscard]] std::expected<byte_type, std::string> at(size_t position) const {
            if (position >= size()) {
                return std::unexpected("bytes index out of range");
            }
            return (*this)[position];
        }

        /** @brief 带边界检查的访问，返回 expected（按值） */
        [[nodiscard]] std::expected<byte_type, std::string> at(size_t position) {
            if (position >= size()) {
                return std::unexpected("bytes index out of range");
            }
            return (*this)[position];
        }

        /** @brief 获取第一个字节的引用,请确保参数数据不为空,以免引起ub */
        byte_type& front() noexcept { return mutable_data()[0]; }
        /** @brief 获取第一个字节的常量引用,请确保参数数据不为空,以免引起ub*/
        const byte_type& front() const noexcept { return data()[0]; }
        /** @brief 获取最后一个字节的引用,请确保参数数据不为空,以免引起ub */
        byte_type& back() noexcept { return mutable_data()[size() - 1]; }
        /** @brief 获取最后一个字节的常量引用,请确保参数数据不为空,以免引起ub */
        const byte_type& back() const noexcept { return data()[size() - 1]; }

        /** @brief 常量下标访问（不检查边界） */
        const byte_type& operator[](size_t position) const { return *(data() + position); }
        /** @brief 非常量下标访问（不检查边界） */
        byte_type& operator[](size_t position) { return *(mutable_data() + position); }

        // ==================== 修改操作 ====================
        /**
         * @brief 追加数据
         * @param data_pointer 数据指针
         * @param data_size 数据大小
         * @return 当前对象引用
         */
        bytes& append(const void* data_pointer, size_t data_size) {
            if (data_size == 0) return *this;
            const byte_type* byte_data = reinterpret_cast<const byte_type*>(data_pointer);
            auto old_size = size();
            auto old_data = data();
            auto new_data_ptr = expand_without_initialization(data_size, true);
            // 处理别名情况（源数据在当前对象内部）
            std::less_equal<const byte_type*> less_equal;
            if (less_equal(old_data, byte_data) && !less_equal(old_data + old_size, byte_data)) {
                byte_data = data() + (byte_data - old_data);
                std::memmove(new_data_ptr, byte_data, data_size);
            }
            else {
                std::memcpy(new_data_ptr, byte_data, data_size);
            }
            return *this;
        }

        /** @brief 追加字节序列 */
        bytes& append(const bytes& data_to_append) {
            return append(data_to_append.data(), data_to_append.size());
        }

        /** @brief 移动追加字节序列 */
        bytes& append(bytes&& data_to_append) {
            return append(data_to_append.data(), data_to_append.size());
        }

        /** @brief 追加初始化列表 */
        bytes& append(std::initializer_list<byte_type> initializer_list) {
            return append(initializer_list.begin(), initializer_list.size());
        }

        /** @brief 从迭代器范围追加（迭代器类型为 byte_type*） */
        template <typename InputIterator>
        bytes& append(InputIterator begin_iterator, InputIterator end_iterator) {
            size_t data_size = static_cast<size_t>(std::distance(begin_iterator, end_iterator));
            auto new_data_ptr = expand_without_initialization(data_size, true);
            std::copy(begin_iterator, end_iterator, new_data_ptr);
            return *this;
        }

        /** @brief 在末尾添加一个字节 */
        void push_back(byte_type byte_value) {
            *expand_without_initialization(1, true) = byte_value;
        }

        /**
         * @brief 赋值操作
         * @param source_data 源数据指针
         * @param data_size 数据大小
         * @return 当前对象引用
         */
        bytes& assign(const void* source_data, size_t data_size) {
            if (data_size == 0) {
                clear();
            }
            else if (size() >= data_size) {
                std::memmove(mutable_data(), source_data, data_size);
                shrink_by(size() - data_size);
            }
            else {
                clear();
                std::memcpy(expand_without_initialization(data_size), source_data, data_size);
            }
            return *this;
        }

        /** @brief 移动赋值 */
        bytes& assign(bytes&& data_to_assign) { return *this = std::move(data_to_assign); }
        /** @brief 初始化列表赋值 */
        bytes& assign(std::initializer_list<byte_type> initializer_list) {
            return assign(initializer_list.begin(), initializer_list.size());
        }

        /**
         * fix : 修复了内存重叠的问题   
         * @brief 插入数据
         * @param position 插入位置
         * @param data_pointer 数据指针
         * @param data_size 数据大小
         * @return 当前对象引用
         */
        bytes& insert(size_t position, const void* data_pointer, size_t data_size) {
            if (data_size == 0) return *this;
            if (position >= size()) return append(data_pointer, data_size);

            // 获取当前信息
            const byte_type* old_data = data();
            size_t old_size = size();
            const byte_type* src = reinterpret_cast<const byte_type*>(data_pointer);

            // 判断源数据是否落在当前对象内部
            std::less_equal<const byte_type*> less_equal;
            bool is_alias = less_equal(old_data, src) && !less_equal(old_data + old_size, src);

            // 如果需要扩容，先执行（但不直接写入新数据）
            // 注意：expand_without_initialization 可能重新分配内存，使 old_data 失效
            // 因此必须在扩容前保存偏移（若为别名）
            size_t src_offset = 0;
            if (is_alias) {
                src_offset = src - old_data;  // 相对于原始起始的偏移
            }

            // 扩容（此时指针可能改变）
            expand_without_initialization(data_size, true);

            // 获取新缓冲区
            byte_type* buf = mutable_data();
            size_t new_size = size(); // 此时已包含新增大小

            // 移动原有数据（从 position 开始的后半段）向后腾出位置
            std::memmove(buf + position + data_size, buf + position, old_size - position);

            // 现在写入新数据
            const byte_type* src_final = src;
            if (is_alias) {
                // 源数据在当前对象内部，且已经随扩容移动，修正指针
                src_final = buf + src_offset;
            }
            std::memcpy(buf + position, src_final, data_size);

            return *this;
        }


        /** @brief 插入字节序列 */
        bytes& insert(size_t position, const bytes& data_to_insert) {
            return insert(position, data_to_insert.data(), data_to_insert.size());
        }

        /** @brief 移动插入字节序列 */
        bytes& insert(size_t position, bytes&& data_to_insert) {
            return insert(position, data_to_insert.data(), data_to_insert.size());
        }

        /** @brief 插入初始化列表 */
        bytes& insert(size_t position, std::initializer_list<byte_type> initializer_list) {
            return insert(position, initializer_list.begin(), initializer_list.size());
        }

        /** @brief 在迭代器位置插入（迭代器类型为 const byte_type*） */
        template <typename ForwardIterator>
        bytes& insert(const byte_type* iterator_position, ForwardIterator begin_iterator, ForwardIterator end_iterator) {
            size_t position = iterator_position - cbegin();
            size_t data_size = static_cast<size_t>(std::distance(begin_iterator, end_iterator));
            auto old_size = size();
            expand_without_initialization(data_size, true);
            auto buffer_ptr = mutable_data();
            std::memmove(buffer_ptr + position + data_size, buffer_ptr + position, old_size - position);
            std::copy(begin_iterator, end_iterator, buffer_ptr + position);
            return *this;
        }

        /**
         * @brief 替换指定范围的字节
         * @param start_position 起始位置
         * @param replace_length 要替换的字节数
         * @param new_data_pointer 新数据指针
         * @param new_data_size 新数据大小
         * @return 当前对象引用
         */
        bytes& replace(size_t start_position, size_t replace_length,
            const void* new_data_pointer, size_t new_data_size) {
            if (empty()) {
                return *this; 
            }
            if (start_position > size()) start_position = size() - 1;
            if (start_position + replace_length > size()) replace_length = size() - start_position;
            size_t new_total_size = size() - replace_length + new_data_size;
            if (start_position == 0) {
                if (new_data_size == 0) {
                    bytes temp(begin() + replace_length, new_total_size);
                    swap(temp);
                }
                else {
                    bytes temp(new_data_pointer, new_data_size);
                    temp.append(begin() + replace_length, size() - replace_length);
                    swap(temp);
                }
            }
            else {
                bytes temp(begin(), start_position);
                if (new_data_size > 0) temp.append(new_data_pointer, new_data_size);
                if (start_position + new_data_size < new_total_size)
                    temp.append(begin() + start_position + replace_length, size() - start_position - replace_length);
                swap(temp);
            }
            return *this;
        }

        /** @brief 替换为字节序列 */
        bytes& replace(size_t start_position, size_t replace_length, const bytes& replacement_data = {}) {
            return replace(start_position, replace_length, replacement_data.data(), replacement_data.size());
        }

        /**
         * @brief 子序列替换
         * @param old_subsequence 要被替换的子序列
         * @param new_subsequence 用作替换的子序列
         * @param start_index 起始搜索位置
         * @param max_replace_count 最大替换次数
         * @return 当前对象引用
         */
        bytes& replace_sub(const bytes& old_subsequence, const bytes& new_subsequence = {},
            size_t start_index = 0,
            size_t max_replace_count = std::numeric_limits<size_t>::max()) {
            if (empty() || old_subsequence.empty()) return *this;
            const auto* source_data = data();
            size_t source_length = size();
            const auto* target_data = old_subsequence.data();
            size_t target_length = old_subsequence.size();
            if (start_index >= source_length || target_length > source_length) return *this;
            const auto* replacement_data = new_subsequence.empty() ? nullptr : new_subsequence.data();
            size_t replacement_length = new_subsequence.size();
            bytes result;
            const auto* first_match = source_data;
            const auto* search_start = source_data + start_index;
            size_t remaining_length = source_length;
            for (; max_replace_count > 0; --max_replace_count) {
                const auto  position = internal_find(search_start, remaining_length, target_data, target_length);
                if (!position.has_value()) break;
                if (search_start + position.value() > first_match) {
                    result.append(first_match, search_start + position.value() - first_match);
                }
                if (replacement_length > 0) {
                    result.append(replacement_data, replacement_length);
                }
                search_start += position.value() + target_length;
                first_match = search_start;
                remaining_length -= position.value() + target_length;
            }
            if (source_data + source_length - first_match > 0) {
                result.append(first_match, source_data + source_length - first_match);
            }
            *this = std::move(result);
            return *this;
        }

        /**
         * @brief 反转指定范围的字节序（按类型大小分组反转）
         * @return 成功时返回 this 指针，失败时返回错误信息
         */
        template <typename T>
        [[nodiscard]] std::expected<bytes*, std::string> reverse_endianness(size_t start_offset = 0, size_t end_offset = maxbytes_count) {
            static_assert(std::is_arithmetic_v<T>, "T must be arithmetic type");
            if (end_offset == maxbytes_count || end_offset > size()) end_offset = size();
            size_t total_bytes = end_offset - start_offset;
            constexpr size_t type_size = sizeof(T);
            if (total_bytes % type_size != 0) {
                return std::unexpected("range size must be a multiple of type size");
            }
            for (size_t i = start_offset; i < end_offset; i += type_size) {
                std::reverse(begin() + i, begin() + i + type_size);
            }
            return this;
        }

        /** @brief 反转整个字节序列 */
        bytes& reverse() {
            std::reverse(begin(), end());
            return *this;
        }

        // ==================== 查找操作 ====================
        /** @brief 正向查找子序列 */
        [[nodiscard]] std::optional<size_t> find(const bytes& pattern, size_t start_offset = 0) const {
            return internal_find(data(), size(), pattern.data(), pattern.size(), start_offset);
        }

        /** @brief 反向查找子序列 */
        [[nodiscard]] std::optional<size_t> rfind(const bytes& pattern, size_t start_offset = std::numeric_limits<size_t>::max()) const {
            return internal_reverse_find(data(), size(), pattern.data(), pattern.size(), start_offset);
        }

        // ==================== 子序列操作 ====================
        /** @brief 取左侧子序列 */
        [[nodiscard]] bytes left(size_t count) const {
            if (empty() || count == 0) return {};
            return bytes(data(), std::min(count, size()));
        }

        /** @brief 取右侧子序列 */
        [[nodiscard]] bytes right(size_t count) const {
            if (empty() || count == 0) return {};
            count = std::min(count, size());
            return bytes(data() + size() - count, count);
        }

        /** @brief 取中间子序列 */
        [[nodiscard]] bytes slice_copy(size_t start_position, size_t count = std::numeric_limits<size_t>::max()) const {
            if (empty() || start_position >= size()) return {};
            start_position = std::min(start_position, size());
            count = std::min(count, size() - start_position);
            return bytes(data() + start_position, count);
        }

        /** @brief 取左侧视图 */
        [[nodiscard]] view left_view(size_t n) const {
            return view(data(), std::min(n, size()));
        }

        /** @brief 取中间视图 */
        [[nodiscard]] view slice(size_t pos, size_t n = std::numeric_limits<size_t>::max()) const {
            if (pos >= size()) return { nullptr, 0 };
            return view(data() + pos, std::min(n, size() - pos));
        }

        /** @brief 取右侧视图 */
        [[nodiscard]] view right_view(size_t n) const {
            if (n >= size()) return view(data(), size());
            return view(data() + (size() - n), n);
        }

        /** @brief 取子序列（mid 的别名） */
        [[nodiscard]] bytes subbytes(size_t start_position, size_t count = std::numeric_limits<size_t>::max()) const {
            return slice_copy(start_position, count);
        }

        /**
         * @brief 分割字节序列
         * @param separator 分隔符字节序列
         * @param max_split_count 最大分割次数
         * @return 分割后的字节序列数组
         */
        [[nodiscard]] std::vector<bytes> split(const bytes& separator = { 0 },
            size_t max_split_count = std::numeric_limits<size_t>::max()) const {
            if (empty() || separator.empty() || max_split_count == 0) return {};
            std::vector<bytes> result;
            const byte_type* source_data = data();
            size_t source_length = size();
            size_t separator_length = separator.size();
            const byte_type* current_position = source_data;
            const byte_type* end_position = source_data + source_length;
            for (size_t count = 0; count < max_split_count - 1 && current_position < end_position; ++count) {
                const auto position = internal_find(current_position, end_position - current_position,
                    separator.data(), separator_length, 0);
                if (!position.has_value()) break;
                result.emplace_back(current_position, current_position + position.value());
                current_position += position.value() + separator_length;
            }
            if (current_position < end_position) {
                result.emplace_back(current_position, end_position);
            }
            return result;
        }

        // ==================== 转换操作 ====================
        /** @brief 转换为十进制文本格式 */
        template<typename CharType = char>
        [[nodiscard]] std::basic_string<CharType> decimal() const {
            return _conversion::to_decimal<CharType>(data(), size());
        }

        /** @brief 转换为十六进制文本 */
        template<typename CharType = char>
        [[nodiscard]] std::basic_string<CharType> hex(bool use_lowercase = true) const {
            return _conversion::to_hex<CharType>(data(), size(), use_lowercase);
        }

        /** @brief Base64 编码 */
        template<typename CharType = char>
        [[nodiscard]] std::basic_string<CharType> base64(
            const std::basic_string<CharType>& alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") {
            return _conversion::to_base64<CharType>(data(), size(), alphabet);
        }

        /**
         * @brief 提取指定类型的数据（直接内存复制）
         * @tparam T 数据类型，必须是可平凡复制的
         * @param offset 偏移量
         * @return 转换后的数据
         */
        template<typename T>
        [[nodiscard]] T extract_data(size_t offset) const {
            static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
            T value{};
            if (offset + sizeof(T) <= size()) {
                std::memcpy(&value, data() + offset, sizeof(T));
            }
            return value;
        }

        /**
         * @brief 从指定偏移处读取一个数值（支持字节序转换）
         * @return 成功时返回数值，失败时返回错误信息
         */
        template <typename T>
        [[nodiscard]] std::expected<T, std::string> extract_num(size_t offset, Endianness endian = Endianness::Native) const {
            static_assert(std::is_arithmetic_v<T>, "T must be arithmetic type");
            if (offset + sizeof(T) > size()) {
                return std::unexpected("extract_num: read out of range");
            }
            T value;
            std::memcpy(&value, data() + offset, sizeof(T));
            if constexpr (sizeof(T) > 1) {
                if (endian != Endianness::Native) {
                    bool native_is_little = (std::endian::native == std::endian::little);
                    bool target_is_little = (endian == Endianness::LittleEndian);
                    if (native_is_little != target_is_little) {
                        value = _conversion::swap_endian(value);
                    }
                }
            }
            return value;
        }

        // ==================== 文件操作 ====================
        /**
         * @brief 写入文件
         * @tparam CharType 字符类型（文件名编码）
         * @param filename 文件名
         * @return true 如果写入成功
         */
        template<typename CharType = char>
        bool write_to_file(const std::basic_string<CharType>& filename) const {
            std::basic_ofstream<CharType> file(filename, std::ios::binary);
            if (!file) return false;
            if (!empty()) {
                file.write(reinterpret_cast<const CharType*>(data()),
                    static_cast<std::streamsize>(size()));
            }
            return file.good();
        }

        /** @brief 写入文件（C 风格字符串版本） */
        template<typename CharType>
        bool write_to_file(const CharType* filename_cstring) {
            return write_to_file(std::basic_string<CharType>(filename_cstring));
        }

        /** @brief 转换为字符串（按原始字节解释） */
        template<typename CharType = char>
        [[nodiscard]] std::basic_string<CharType> to_string() const {
            return _conversion::to_string<CharType>(data(), size());
        }

        /** @brief 隐式转换为只读 span */
        operator std::span<const uint8_t>() const noexcept {
            return { data(), size() };
        }

        // ==================== 操作符 ====================
        /** @brief 追加操作符 */
        bytes& operator+=(const bytes& data_to_append) { return append(data_to_append); }
        /** @brief 移动追加操作符 */
        bytes& operator+=(bytes&& data_to_append) { return append(std::move(data_to_append)); }
        /** @brief 初始化列表追加 */
        bytes& operator+=(std::initializer_list<byte_type> initializer_list) {
            return append(initializer_list.begin(), initializer_list.size());
        }

        // ==================== 静态工厂方法 ====================
        /**
         * @brief 从 Base64 字符串解码（严格模式）
         * @return 成功返回 bytes（可能为空），失败返回错误信息
         */
        template<typename CharType = char>
        [[nodiscard]] static std::expected<bytes, std::string> from_base64(
            const std::basic_string<CharType>& base64_string,
            bool remove_padding,
            const std::basic_string<CharType>& alphabet) {
            if (base64_string.empty()) return bytes{};
            auto is_ascii = [](CharType ch) -> bool {
                using UT = std::make_unsigned_t<CharType>;
                return static_cast<UT>(ch) <= 255;
                };
            if (alphabet.size() != 64) {
                return std::unexpected("Base64 alphabet must be exactly 64 characters");
            }
            byte_type decode_table[256] = { 0 };
            bool table_filled[256] = { false };
            for (size_t i = 0; i < alphabet.size(); ++i) {
                unsigned char ch = static_cast<unsigned char>(alphabet[i]);
                if (!is_ascii(ch)) {
                    return std::unexpected("Base64 alphabet contains non-ASCII character");
                }
                decode_table[ch] = static_cast<byte_type>(i);
                table_filled[ch] = true;
            }
            // 检查非法字符
            for (CharType ch : base64_string) {
                unsigned char uc = static_cast<unsigned char>(ch);
                if (uc == '=') continue;
                if (!table_filled[uc]) {
                    return std::unexpected("Base64 string contains invalid character");
                }
            }
            size_t input_length = base64_string.size();
            size_t padding_count = 0;
            if (input_length >= 2 && base64_string[input_length - 1] == '=') {
                padding_count++;
                if (input_length >= 2 && base64_string[input_length - 2] == '=') padding_count++;
            }
            size_t output_length = (input_length * 3) / 4 - padding_count;
            bytes result(output_length);
            const CharType* input_data = base64_string.data();
            byte_type* output_data = result.mutable_data();
            size_t output_index = 0;
            uint32_t buffer = 0;
            int bits_collected = 0;
            for (size_t i = 0; i < input_length; ++i) {
                CharType character = input_data[i];
                if (character == '=') break;
                byte_type value = decode_table[static_cast<unsigned char>(character)];
                buffer = (buffer << 6) | value;
                bits_collected += 6;
                if (bits_collected >= 8) {
                    bits_collected -= 8;
                    output_data[output_index++] = static_cast<byte_type>((buffer >> bits_collected) & 0xFF);
                }
            }
            if (remove_padding) result.resize(output_index);
            return result;
        }


        /**
         * @brief 从 Base64 字符串解码（严格模式）
         * @return 成功返回 bytes（可能为空），失败返回错误信息
        */
        template<typename CharType = char>
        [[nodiscard]] static std::expected<bytes, std::string> from_base64(
            const std::basic_string<CharType>& base64_string,
            bool remove_padding = true) {
            if (base64_string.empty()) return bytes{};
            constexpr byte_type decode_table[] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,62,0,0,0,63,52,53,54,55,56,57,58,59,60,61,0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,0,0,0,0,0,0,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
            constexpr bool table_filled[256] =   { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };


            // 检查非法字符
            for (CharType ch : base64_string) {
                unsigned char uc = static_cast<unsigned char>(ch);
                if (uc == '=') continue;
                if (!table_filled[uc]) {
                    return std::unexpected("Base64 string contains invalid character");
                }
            }
            size_t input_length = base64_string.size();
            size_t padding_count = 0;
            if (input_length >= 2 && base64_string[input_length - 1] == '=') {
                padding_count++;
                if (input_length >= 2 && base64_string[input_length - 2] == '=') padding_count++;
            }
            size_t output_length = (input_length * 3) / 4 - padding_count;
            bytes result(output_length);
            const CharType* input_data = base64_string.data();
            byte_type* output_data = result.mutable_data();
            size_t output_index = 0;
            uint32_t buffer = 0;
            int bits_collected = 0;
            for (size_t i = 0; i < input_length; ++i) {
                CharType character = input_data[i];
                if (character == '=') break;
                byte_type value = decode_table[static_cast<unsigned char>(character)];
                buffer = (buffer << 6) | value;
                bits_collected += 6;
                if (bits_collected >= 8) {
                    bits_collected -= 8;
                    output_data[output_index++] = static_cast<byte_type>((buffer >> bits_collected) & 0xFF);
                }
            }
            if (remove_padding) result.resize(output_index);
            return result;
        }

        /** @brief 从 Base64 字符串解码（C 风格字符串版本） */
        template<typename CharType = char>
        [[nodiscard]] static std::expected<bytes, std::string> from_base64(
            const CharType* base64_cstring,
            bool remove_padding = true,
            const CharType* alphabet_cstring = nullptr) {
            if (base64_cstring == nullptr) return bytes{};
            if (alphabet_cstring == nullptr)
            {
                return from_base64(std::basic_string<CharType>(base64_cstring),
                    remove_padding);
            }
            return from_base64(std::basic_string<CharType>(base64_cstring),
                remove_padding, std::basic_string<CharType>(alphabet_cstring));
        }

        /**
         * @brief 从十六进制字符串解码（严格模式）
         * @return 成功返回 bytes（可能为空），失败返回错误信息
         */
        template<typename CharType = char>
        [[nodiscard]] static std::expected<bytes, std::string> from_hex(const std::basic_string<CharType>& hex_string) {
            if (hex_string.empty()) return bytes{};
            byte_type hex_map[256] = { 0 };
            bool is_hex[256] = { false };
            for (int i = 0; i < 10; ++i) { hex_map['0' + i] = static_cast<byte_type>(i); is_hex['0' + i] = true; }
            for (int i = 0; i < 6; ++i) {
                hex_map['A' + i] = static_cast<byte_type>(10 + i); is_hex['A' + i] = true;
                hex_map['a' + i] = static_cast<byte_type>(10 + i); is_hex['a' + i] = true;
            }
            size_t valid_count = 0;
            for (CharType ch : hex_string) {
                unsigned char uc = static_cast<unsigned char>(ch);
                if (!is_hex[uc]) {
                    return std::unexpected("Hex string contains invalid character");
                }
                valid_count++;
            }
            if (valid_count % 2 != 0) {
                return std::unexpected("Hex string length must be even");
            }
            bytes result(valid_count / 2);
            byte_type* output_data = result.mutable_data();
            size_t output_index = 0;
            byte_type high_nibble = 0;
            bool has_high_nibble = false;
            for (CharType ch : hex_string) {
                unsigned char uc = static_cast<unsigned char>(ch);
                byte_type value = hex_map[uc];
                if (!has_high_nibble) {
                    high_nibble = value << 4;
                    has_high_nibble = true;
                }
                else {
                    output_data[output_index++] = high_nibble | value;
                    has_high_nibble = false;
                }
            }
            return result;
        }

        /** @brief 从十六进制字符串解码（C 风格字符串版本） */
        template<typename CharType>
        [[nodiscard]] static std::expected<bytes, std::string> from_hex(const CharType* hex_cstring) {
            return from_hex(std::basic_string<CharType>(hex_cstring));
        }

        /**
         * @brief 生成随机字节序列（总是成功，不会失败）
         * @param size 字节序列大小
         * @return 随机字节序列
         */
        [[nodiscard]] static bytes from_random(size_t size) {
            bytes result(size);
            std::random_device random_device;
            std::mt19937_64 generator(random_device());
            std::uniform_int_distribution<uint16_t> distribution(0, 255);
            byte_type* data_ptr = result.mutable_data();
            for (size_t i = 0; i < size; ++i) data_ptr[i] = static_cast<byte_type>(distribution(generator));
            return result;
        }

        /**
         * @brief 从文件读取全部内容
         * @return 成功返回 bytes（可能为空），失败返回错误信息
         */
        template<typename CharType = char>
        [[nodiscard]] static std::expected<bytes, std::string> from_file(const std::basic_string<CharType>& filename) {
            std::basic_ifstream<CharType> file(filename, std::ios::binary | std::ios::ate);
            if (!file) {
                return std::unexpected("cannot open file");
            }
            std::streamsize file_size = file.tellg();
            if (file_size < 0) {
                return std::unexpected("failed to get file size");
            }
            file.seekg(0, std::ios::beg);
            bytes result(static_cast<size_t>(file_size));
            if (file_size > 0) {
                file.read(reinterpret_cast<CharType*>(result.mutable_data()), file_size);
                if (!file) {
                    return std::unexpected("failed to read file content");
                }
            }
            return result;
        }

        /** @brief 从文件读取全部内容（C 风格字符串版本） */
        template<typename CharType>
        [[nodiscard]] static std::expected<bytes, std::string> from_file(const CharType* filename_cstring) {
            return from_file(std::basic_string<CharType>(filename_cstring));
        }

        /**
         * @brief 从文件的部分区域读取内容（支持偏移和长度限制）
         * @param filename 文件名（窄字符字符串）
         * @param offset 起始偏移量（字节），默认为 0
         * @param max_size 最大读取字节数，默认为 npos 表示读到文件末尾
         * @return 成功返回 bytes（可能为空），失败返回错误信息
         */
        [[nodiscard]] static std::expected<bytes, std::string> from_file(const std::string& filename,
            size_t offset = 0,
            size_t max_size = maxbytes_count) {
            std::ifstream file(filename, std::ios::binary | std::ios::ate);
            if (!file) {
                return std::unexpected("cannot open file");
            }
            std::streamsize file_size = file.tellg();
            if (file_size < 0) {
                return std::unexpected("failed to get file size");
            }
            if (offset >= static_cast<size_t>(file_size)) {
                return bytes{};  // empty but successful
            }
            size_t to_read = (max_size == maxbytes_count) ? static_cast<size_t>(file_size) - offset
                : std::min(max_size, static_cast<size_t>(file_size) - offset);
            bytes result(to_read);
            if (to_read > 0) {
                file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                if (!file) {
                    return std::unexpected("failed to seek file offset");
                }
                file.read(reinterpret_cast<char*>(result.mutable_data()), to_read);
                if (!file) {
                    return std::unexpected("failed to read file content");
                }
            }
            return result;
        }

    private:
        // ==================== 私有类型定义 ====================
        /**
         * @brief 数据存储类别枚举
         */
        enum class StorageCategory : uint8_t {
            isSmall = 0,                                   ///< 小数据（SSO优化）
            isMedium = kIsLittleEndian ? 0x80 : 0x2,       ///< 中等数据（独占堆分配）
            isLarge = kIsLittleEndian ? 0x40 : 0x1,        ///< 大数据（引用计数共享）
        };



        /**
         * @brief 引用计数块结构
         */
        struct RefCountedBlock {
            std::atomic<size_t> reference_count;  ///< 原子引用计数
            byte_type data_start[1];              ///< 数据起始位置（柔性数组）

            static constexpr size_t data_offset() {
                return offsetof(RefCountedBlock, data_start);
            }

            static RefCountedBlock* from_data_pointer(byte_type* data_pointer) {
                return reinterpret_cast<RefCountedBlock*>(
                    reinterpret_cast<byte_type*>(data_pointer) - data_offset());
            }

            static size_t get_reference_count(byte_type* data_pointer) {
                return from_data_pointer(data_pointer)->reference_count.load(std::memory_order_acquire);
            }

            static void increment_reference_count(byte_type* data_pointer) {
                from_data_pointer(data_pointer)->reference_count.fetch_add(1, std::memory_order_acq_rel);
            }

            static void decrement_reference_count(byte_type* data_pointer) {
                auto block_ptr = from_data_pointer(data_pointer);
                size_t old_count = block_ptr->reference_count.fetch_sub(1, std::memory_order_acq_rel);
                if (old_count == 1) {
                    std::free(block_ptr);
                }
            }

            static RefCountedBlock* create_block(size_t* requested_size) {
                size_t capacity_bytes;
                if (!erpc_imp::safe::calculate::checked_add(&capacity_bytes, *requested_size, size_t(1))) {
                    throw std::length_error("capacity overflow");
                }
                if (!erpc_imp::safe::calculate::checked_muladd(&capacity_bytes, capacity_bytes,
                    sizeof(byte_type), data_offset())) {
                    throw std::length_error("capacity overflow");
                }
                auto block_ptr = static_cast<RefCountedBlock*>(erpc_imp::safe::memory::krnln_malloc(capacity_bytes));
                block_ptr->reference_count.store(1, std::memory_order_release);
                *requested_size = (capacity_bytes - data_offset()) / sizeof(byte_type) - 1;
                return block_ptr;
            }

            static RefCountedBlock* create_block_with_data(const byte_type* source_data, size_t* requested_size) {
                const size_t original_size = *requested_size;
                auto block_ptr = create_block(requested_size);
                if (original_size > 0) {
                    std::memcpy(block_ptr->data_start, source_data, original_size);
                }
                return block_ptr;
            }

            static RefCountedBlock* reallocate_block(byte_type* old_data_pointer, size_t current_size,
                size_t current_capacity, size_t* new_capacity) {
                size_t capacity_bytes;
                if (!erpc_imp::safe::calculate::checked_add(&capacity_bytes, *new_capacity, size_t(1))) {
                    throw std::length_error("capacity overflow");
                }
                if (!erpc_imp::safe::calculate::checked_muladd(&capacity_bytes, capacity_bytes,
                    sizeof(byte_type), data_offset())) {
                    throw std::length_error("capacity overflow");
                }
                auto old_block_ptr = from_data_pointer(old_data_pointer);
                auto new_block_ptr = static_cast<RefCountedBlock*>(
                    erpc_imp::safe::memory::smart_realloc(old_block_ptr,
                        data_offset() + (current_size + 1) * sizeof(byte_type),
                        data_offset() + (current_capacity + 1) * sizeof(byte_type),
                        capacity_bytes));
                *new_capacity = (capacity_bytes - data_offset()) / sizeof(byte_type) - 1;
                return new_block_ptr;
            }
        };
        /**
         * @brief 中等或大数据存储结构
         */
        struct alignas(void*) MediumLargeStorage {
            byte_type* data_ptr;          ///< 指向堆分配数据的指针
            size_t current_size;           ///< 当前有效数据大小
            size_t capacity_with_flags;     ///< 容量（包含类别标记位）

            [[nodiscard]] size_t actual_capacity() const {
                return kIsLittleEndian ?
                    capacity_with_flags & kCapacityExtractMask :
                    capacity_with_flags >> 2;
            }

            void set_capacity_with_category(size_t capacity, StorageCategory category) {
                capacity_with_flags = kIsLittleEndian ?
                    capacity | (static_cast<size_t>(category) << kCategoryShift) :
                    (capacity << 2) | static_cast<size_t>(category);
            }
        };

        // ==================== 私有常量 ====================
        static constexpr size_t kLastByteIndex = sizeof(MediumLargeStorage) - 1;
        static constexpr size_t kMaxSmallSize = kLastByteIndex / sizeof(byte_type);
        static constexpr size_t kMaxMediumSize = 0xFF / sizeof(byte_type) - sizeof(byte_type);
        static constexpr byte_type kCategoryExtractMask = kIsLittleEndian ? 0xC0 : 0x3;
        static constexpr size_t kCategoryShift = (sizeof(size_t) - 1) * 8;
        static constexpr size_t kCapacityExtractMask = kIsLittleEndian ?
            ~(static_cast<size_t>(kCategoryExtractMask) << kCategoryShift) : 0x0;

        // 用于 view 和外部接口的 npos（保留）
        static constexpr size_t maxbytes_count = std::numeric_limits<size_t>::max();

        

        // ==================== 私有成员变量 ====================
        // 内存布局
        union alignas(void*) {
            byte_type raw_bytes[sizeof(MediumLargeStorage)];      ///< 字节访问视图
            byte_type small_data[sizeof(MediumLargeStorage)];              ///< 小数据存储区
            MediumLargeStorage medium_large;                       ///< 中大数据存储结构
        };

        // 静态断言（英文）
        static_assert(sizeof(byte_type) == sizeof(uint8_t), "MediumLargeStorage memory layout corruption");
        static_assert(sizeof(MediumLargeStorage) % sizeof(byte_type) == 0, "MediumLargeStorage memory layout corruption");
        static_assert(offsetof(MediumLargeStorage, data_ptr) == 0, "MediumLargeStorage memory layout corruption");
        static_assert(offsetof(MediumLargeStorage, current_size) == sizeof(medium_large.data_ptr), "MediumLargeStorage memory layout corruption");
        static_assert(offsetof(MediumLargeStorage, capacity_with_flags) == 2 * sizeof(medium_large.data_ptr), "MediumLargeStorage memory layout corruption");
        static_assert(alignof(MediumLargeStorage) >= alignof(byte_type), "Alignment requirement not met");
        static_assert(std::is_trivially_destructible_v<MediumLargeStorage>, "MediumLargeStorage must be trivially destructible");

        // ==================== 私有辅助方法 ====================
        [[nodiscard]] bool is_small() const { return storage_category() == StorageCategory::isSmall; }
        [[nodiscard]] bool is_med() const {
            auto cat = storage_category();
            return (static_cast<uint8_t>(cat) & static_cast<uint8_t>(StorageCategory::isMedium)) != 0;
        }
        [[nodiscard]] bool is_lrg() const {
            auto cat = storage_category();
            return (static_cast<uint8_t>(cat) & static_cast<uint8_t>(StorageCategory::isLarge)) != 0;
        }

        [[nodiscard]] StorageCategory storage_category() const noexcept {
            return static_cast<StorageCategory>(raw_bytes[kLastByteIndex] & kCategoryExtractMask);
        }

        [[nodiscard]] size_t small_storage_size() const noexcept {
            constexpr auto shift = kIsLittleEndian ? 0 : 2;
            auto small_shifted = static_cast<size_t>(small_data[kMaxSmallSize]) >> shift;
            return kMaxSmallSize - small_shifted;
        }

        void set_small_storage_size(size_t new_size) noexcept {
            constexpr auto shift = kIsLittleEndian ? 0 : 2;
            small_data[kMaxSmallSize] = static_cast<byte_type>((kMaxSmallSize - new_size) << shift);
        }

        void reset_to_empty() noexcept { set_small_storage_size(0); }

        void destroy_medium_large_storage() noexcept {
            if (is_med()) std::free(medium_large.data_ptr);
            else if (is_lrg()) RefCountedBlock::decrement_reference_count(medium_large.data_ptr);
        }

        byte_type* expand_without_initialization(size_t growth_amount, bool use_exponential_growth = false) {
            size_t old_size, new_size;
            if (storage_category() == StorageCategory::isSmall) {
                old_size = small_storage_size();
                new_size = old_size + growth_amount;
                if (new_size <= kMaxSmallSize) {
                    set_small_storage_size(new_size);
                    return small_data + old_size;
                }
                reserve_small_storage(use_exponential_growth ? std::max(new_size, 2 * kMaxSmallSize) : new_size);
            }
            else {
                old_size = medium_large.current_size;
                new_size = old_size + growth_amount;
                if (new_size > capacity()) {
                    reserve(use_exponential_growth ? std::max(new_size, 1 + capacity() * 3 / 2) : new_size);
                }
            }
            medium_large.current_size = new_size;
            return medium_large.data_ptr + old_size;
        }

        void shrink_by(size_t shrink_amount) {
            if (shrink_amount == 0) return;
            if (is_small()) {
                set_small_storage_size(small_storage_size() - shrink_amount);
            }
            else if (is_med() || RefCountedBlock::get_reference_count(medium_large.data_ptr) == 1) {
                medium_large.current_size -= shrink_amount;
            }
            else {
                bytes(medium_large.data_ptr, medium_large.current_size - shrink_amount).swap(*this);
            }
        }

        void initialize_small_storage(const byte_type* source_data, size_t data_size) {
            if (data_size > 0) std::memcpy(small_data, source_data, data_size);
            set_small_storage_size(data_size);
        }

        void initialize_medium_storage(const byte_type* source_data, size_t data_size) {
            medium_large.data_ptr = static_cast<byte_type*>(erpc_imp::safe::memory::krnln_malloc(data_size));
            if (data_size > 0) std::memcpy(medium_large.data_ptr, source_data, data_size);
            medium_large.current_size = data_size;
            medium_large.set_capacity_with_category(data_size, StorageCategory::isMedium);
        }

        void initialize_large_storage(const byte_type* source_data, size_t data_size) {
            size_t effective_capacity = data_size;
            auto new_block = RefCountedBlock::create_block_with_data(source_data, &effective_capacity);
            medium_large.data_ptr = new_block->data_start;
            medium_large.current_size = data_size;
            medium_large.set_capacity_with_category(effective_capacity, StorageCategory::isLarge);
        }

        void copy_small_storage(const bytes& source) { medium_large = source.medium_large; }

        void copy_medium_storage(const bytes& source) {
            medium_large.data_ptr = static_cast<byte_type*>(erpc_imp::safe::memory::krnln_malloc(source.medium_large.current_size));
            std::memcpy(medium_large.data_ptr, source.medium_large.data_ptr, source.medium_large.current_size);
            medium_large.current_size = source.medium_large.current_size;
            medium_large.set_capacity_with_category(source.medium_large.current_size, StorageCategory::isMedium);
        }

        void copy_large_storage(const bytes& source) {
            medium_large = source.medium_large;
            RefCountedBlock::increment_reference_count(medium_large.data_ptr);
        }

        void reserve_small_storage(size_t min_capacity) {
            if (min_capacity <= kMaxSmallSize) return;
            size_t current_size = small_storage_size();
            if (min_capacity <= kMaxMediumSize) {
                auto new_data_ptr = static_cast<byte_type*>(erpc_imp::safe::memory::krnln_malloc(min_capacity));
                std::memcpy(new_data_ptr, small_data, current_size);
                medium_large.data_ptr = new_data_ptr;
                medium_large.current_size = current_size;
                medium_large.set_capacity_with_category(min_capacity, StorageCategory::isMedium);
            }
            else {
                size_t capacity = min_capacity;
                auto new_block = RefCountedBlock::create_block(&capacity);
                std::memcpy(new_block->data_start, small_data, current_size);
                medium_large.data_ptr = new_block->data_start;
                medium_large.current_size = current_size;
                medium_large.set_capacity_with_category(capacity, StorageCategory::isLarge);
            }
        }

        void reserve_medium_storage(size_t min_capacity) {
            if (min_capacity <= medium_large.actual_capacity()) return;
            if (min_capacity <= kMaxMediumSize) {
                medium_large.data_ptr = static_cast<byte_type*>(
                    erpc_imp::safe::memory::smart_realloc(medium_large.data_ptr, medium_large.current_size,
                        medium_large.actual_capacity(), min_capacity));
                medium_large.set_capacity_with_category(min_capacity, StorageCategory::isMedium);
            }
            else {
                bytes temporary;
                temporary.reserve(min_capacity);
                temporary.medium_large.current_size = medium_large.current_size;
                std::memcpy(temporary.medium_large.data_ptr, medium_large.data_ptr, medium_large.current_size);
                temporary.swap(*this);
            }
        }

        void reserve_large_storage(size_t min_capacity) {
            if (RefCountedBlock::get_reference_count(medium_large.data_ptr) > 1) {
                unshare(min_capacity);
            }
            else if (min_capacity > medium_large.actual_capacity()) {
                size_t new_capacity = min_capacity;
                auto new_block = RefCountedBlock::reallocate_block(medium_large.data_ptr, medium_large.current_size,
                    medium_large.actual_capacity(), &new_capacity);
                medium_large.data_ptr = new_block->data_start;
                medium_large.set_capacity_with_category(new_capacity, StorageCategory::isLarge);
            }
        }

        void unshare(size_t min_capacity = 0) {
            size_t effective_capacity = std::max(min_capacity, medium_large.actual_capacity());
            auto new_block = RefCountedBlock::create_block(&effective_capacity);
            std::memcpy(new_block->data_start, medium_large.data_ptr, medium_large.current_size);
            RefCountedBlock::decrement_reference_count(medium_large.data_ptr);
            medium_large.data_ptr = new_block->data_start;
            medium_large.set_capacity_with_category(effective_capacity, StorageCategory::isLarge);
        }

        byte_type* get_mutable_large_data() {
            if (RefCountedBlock::get_reference_count(medium_large.data_ptr) > 1) unshare();
            return medium_large.data_ptr;
        }

        static std::optional<size_t> internal_find(const byte_type* haystack, size_t haystack_length,
            const byte_type* needle, size_t needle_length,
            size_t start_offset = 0) {
            if (haystack == nullptr || haystack_length == 0 || needle == nullptr || needle_length == 0)
                return std::nullopt;
            if (needle_length > haystack_length) return std::nullopt;
            if (start_offset > haystack_length) return std::nullopt;
            if (haystack_length - start_offset < needle_length) return std::nullopt; // 避免溢出
            const size_t search_length = haystack_length - start_offset;
            const auto* search_start = haystack + start_offset;
            auto match_iterator = std::search(search_start, search_start + search_length,
                needle, needle + needle_length);
            if (match_iterator != search_start + search_length) {
                return match_iterator - haystack;
            }
            return std::nullopt;
        }

        static std::optional<size_t> internal_reverse_find(const byte_type* source_data, size_t source_length,
            const byte_type* target_pattern, size_t pattern_length,
            size_t max_offset) {
            if (source_data == nullptr || target_pattern == nullptr) return std::nullopt;
            if (max_offset <= source_length) source_length = max_offset;
            if (source_length == 0 || pattern_length == 0 || pattern_length > source_length) return std::nullopt;
            size_t offset = source_length - pattern_length;
            if (pattern_length == 1) {
                for (size_t i = offset; ; --i) {
                    if (source_data[i] == *target_pattern) return i;
                    if (i == 0) break;
                }
                return std::nullopt;
            }
            size_t skip_table[256];
            for (size_t i = 0; i < 256; i++) skip_table[i] = pattern_length;
            for (size_t i = pattern_length; i > 0; i--) skip_table[target_pattern[i - 1]] = i;
            for (const unsigned char* current_address = source_data + offset;
                current_address >= source_data;
                current_address -= skip_table[current_address[-1]]) {
                if (std::memcmp(current_address, target_pattern, pattern_length) == 0) {
                    return current_address - source_data;
                }
            }
            return std::nullopt;
        }

        static std::optional<size_t> internal_reverse_find(const byte_type* source_data, size_t source_length,
            const byte_type* target_pattern, size_t pattern_length) {
            return internal_reverse_find(source_data, source_length, target_pattern, pattern_length, source_length);
        }
    };

    // ==================== 转换辅助函数 ====================
    // 注意：直接转换对于底层实际上是不严谨的（如字符串编码、数值字节序等），
    // 但大部分业务场景可以满足需求。使用时请确保正确性和合理性。

    /** @brief 从平凡类型转换为 membin */
    template<typename T>
    bytes to_bytes(const T& data) {
        static_assert(std::is_trivially_copyable_v<T>, "T must be a trivially copyable type.");
        return bytes(&data, sizeof(T));
    }

    /** @brief 从 C 字符串转换为 membin */
    inline bytes to_bytes(const char* c_string) {
        return bytes(c_string, std::strlen(c_string));
    }

    /** @brief 从宽字符串转换为 membin */
    inline bytes to_bytes(const wchar_t* wide_string) {
        return bytes(wide_string, std::wcslen(wide_string) * sizeof(wchar_t));
    }

    /** @brief 从 UTF-8 字符串转换为 membin */
    inline bytes to_bytes(const char8_t* utf8_string) {
        return bytes(utf8_string, std::char_traits<char8_t>::length(utf8_string) * sizeof(char8_t));
    }

    /** @brief 从 UTF-16 字符串转换为 membin */
    inline bytes to_bytes(const char16_t* utf16_string) {
        return bytes(utf16_string, std::char_traits<char16_t>::length(utf16_string) * sizeof(char16_t));
    }

    /** @brief 从 UTF-32 字符串转换为 membin */
    inline bytes to_bytes(const char32_t* utf32_string) {
        return bytes(utf32_string, std::char_traits<char32_t>::length(utf32_string) * sizeof(char32_t));
    }

    /** @brief 从 std::basic_string 转换为 membin */
    template <typename CharType>
    inline bytes to_bytes(const std::basic_string<CharType>& string) {
        return bytes(string.data(), string.size() * sizeof(CharType));
    }

    /** @brief 从 std::vector 转换为 membin */
    template <typename T>
    bytes to_bytes(const std::vector<T>& vector) {
        if (vector.empty()) return {};
        return bytes(vector.data(), vector.size() * sizeof(T));
    }

    /**
     * @brief 将数值转换为 membin（支持指定字节序）
     * @tparam T 算术类型
     * @param num 需要转换的数值
     * @param order 目标字节序
     */
    template <typename T>
    bytes to_bytes(T num, bytes::Endianness order) {
        static_assert(std::is_arithmetic_v<T>, "必须为可计算类型");
        T data = num;
        if constexpr (std::endian::native == std::endian::little) {
            if (order == bytes::Endianness::BigEndian) data = _conversion::swap_endian(data);
        }
        else {
            if (order == bytes::Endianness::LittleEndian) data = _conversion::swap_endian(data);
        }
        return bytes(&data, sizeof(T));
    }


} // namespace krnln
#endif