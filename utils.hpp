#ifndef KRNLN_UTILS_HPP
#define KRNLN_UTILS_HPP
#include <cstdlib>      
#include <cstring>      
#include <limits>       
#include <type_traits>  
#include <stdexcept>    
#include <new>          
#include <cassert>      

namespace erpc_imp {

    namespace safe {
        namespace memory {

            /**
             * @brief 分配内存并进行空指针检查
             * @param size_bytes 需要分配的字节数
             * @return 指向分配内存的指针
             * @throws std::bad_alloc 如果内存分配失败
             */
            [[nodiscard]] inline void* krnln_malloc(size_t size_bytes) {
                void* ptr = malloc(size_bytes);
                if (!ptr) {
                    throw std::bad_alloc();
                }
                return ptr;
            }

            /**
             * @brief 重新分配内存并进行空指针检查
             * @param ptr 指向现有内存块的指针（可为 nullptr）
             * @param new_size_bytes 新的字节大小
             * @return 指向重新分配内存的指针
             * @throws std::bad_alloc 如果内存重新分配失败
             */
            [[nodiscard]] inline void* krnln_realloc(void* ptr, size_t new_size_bytes) {
                void* new_ptr = realloc(ptr, new_size_bytes);
                if (!new_ptr) {
                    throw std::bad_alloc();
                }
                return new_ptr;
            }

            /**
             * @brief 释放内存（传入 nullptr 安全）
             * @param ptr 指向要释放的内存块的指针
             */
            inline void krnln_free(void* ptr) noexcept {
                free(ptr);
            }

            /**
             * @brief 智能内存重分配，根据空闲空间比例选择复制或原地重分配
             * @param ptr 指向现有内存块的指针（可为 nullptr）
             * @param used_size 已使用字节数（必须 <= current_capacity）
             * @param current_capacity 当前总容量（若 ptr 为 nullptr 则应传 0）
             * @param new_capacity 新容量
             * @return 指向重新分配内存的指针
             * @throws std::bad_alloc 如果内存分配失败
             * @note 当空闲空间超过已用空间的 50% 时，使用复制方式减少内存碎片；
             *       若 new_capacity < used_size，则直接调用 realloc 缩小。
             */
            [[nodiscard]] inline void* smart_realloc(
                void* ptr,
                size_t used_size,
                size_t current_capacity,
                size_t new_capacity)
            {
                size_t slack_space = current_capacity - used_size;
                if (slack_space > (used_size >> 1)) {
                    void* result = krnln_malloc(new_capacity); //没必要检查空,都申请不下内存了可以gg了
                    std::memcpy(result, ptr, used_size);
                    free(ptr);
                    return result;
                }
                return krnln_realloc(ptr, new_capacity);
            }
        }

        namespace calculate {

            /**
             * @brief 整数加法溢出检查（支持有符号和无符号）
             * @tparam T 整数类型
             * @param result_ptr 存储结果的指针
             * @param a 第一个加数
             * @param b 第二个加数
             * @return true 如果加法安全执行，false 如果发生溢出（此时 *result_ptr = 0）
             */
            template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
            [[nodiscard]] bool checked_add(T* result_ptr, T a, T b) {
                if constexpr (std::is_signed_v<T>) {
                    if (a >= 0) {
                        if (std::numeric_limits<T>::max() - a < b) {
                            *result_ptr = T{};
                            return false;
                        }
                    }
                    else if (b < std::numeric_limits<T>::min() - a) {
                        *result_ptr = T{};
                        return false;
                    }
                    *result_ptr = a + b;
                    return true;
                }
                else { //无符号
                    if (a <= std::numeric_limits<T>::max() - b) {
                        *result_ptr = a + b;
                        return true;
                    }
                    *result_ptr = T{};
                    return false;
                }
            }

            /**
             * @brief 无符号整数乘法溢出检查（通用实现）
             * @tparam T 无符号整数类型
             * @param result_ptr 存储结果的指针
             * @param a 第一个乘数
             * @param b 第二个乘数
             * @return true 如果乘法安全执行，false 如果发生溢出（此时 *result_ptr = 0）
             */
            template <typename T, typename = std::enable_if_t<std::is_unsigned_v<T>>>
            [[nodiscard]] bool checked_mul(T* result_ptr, T a, T b) {
                if constexpr (sizeof(T) < sizeof(uint64_t)) {
                    // 较小类型，直接用 uint64_t 计算并检查是否超出 T 的范围
                    const uint64_t result = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
                    if (result > std::numeric_limits<T>::max()) {
                        *result_ptr = T{};
                        return false;
                    }
                    *result_ptr = static_cast<T>(result);
                    return true;
                }
                else {  // sizeof(T) == sizeof(uint64_t)，64位乘法必须分解高位
                    constexpr uint64_t half_bits = 32;
                    constexpr uint64_t half_mask = (1ULL << half_bits) - 1ULL;

                    const uint64_t lhs_high = a >> half_bits;
                    const uint64_t lhs_low = a & half_mask;
                    const uint64_t rhs_high = b >> half_bits;
                    const uint64_t rhs_low = b & half_mask;

                    if (lhs_high == 0 && rhs_high == 0) {
                        *result_ptr = lhs_low * rhs_low;
                        return true;
                    }

                    // 任一高半部分非零，则乘积至少为 2^32 * 2^32 = 2^64，溢出
                    if (lhs_high != 0 && rhs_high != 0) {
                        *result_ptr = T{};
                        return false;
                    }

                    // 一个高半部分非零，计算中间值
                    uint64_t mid_bits1 = lhs_low * rhs_high;
                    uint64_t mid_bits2 = lhs_high * rhs_low;
                    if ((mid_bits1 >> half_bits) != 0 || (mid_bits2 >> half_bits) != 0) {
                        *result_ptr = T{};
                        return false;
                    }

                    uint64_t mid_bits = mid_bits1 + mid_bits2;
                    if (mid_bits >> half_bits != 0) {
                        *result_ptr = T{};
                        return false;
                    }

                    uint64_t low_bits = lhs_low * rhs_low;
                    // 将中间值左移 half_bits 后与低部分相加
                    if (!checked_add(result_ptr, low_bits, mid_bits << half_bits)) {
                        *result_ptr = T{};
                        return false;
                    }
                    return true;
                }
            }

            /**
             * @brief 乘加运算溢出检查 (base * mul + add)
             * @tparam T 无符号整数类型
             * @param result_ptr 存储结果的指针
             * @param base 基数
             * @param mul 乘数
             * @param add 加数
             * @return true 如果运算安全执行，false 如果发生溢出（此时 *result_ptr = 0）
             */
            template <typename T, typename = std::enable_if_t<std::is_unsigned_v<T>>>
            [[nodiscard]] bool checked_muladd(T* result_ptr, T base, T mul, T add) {
                T temp_result{};
                if (!checked_mul(&temp_result, base, mul)) {
                    *result_ptr = T{};
                    return false;
                }
                if (!checked_add(&temp_result, temp_result, add)) {
                    *result_ptr = T{};
                    return false;
                }
                *result_ptr = temp_result;
                return true;
            }
        }
    }
} // namespace krnln

#endif