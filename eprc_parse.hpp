#ifndef EPRC_PARSE_BYTES_HPP
#define EPRC_PARSE_BYTES_HPP

/*
 * eRPC 协议解析器
 * - 构造器（打包为碎片）
 * - 解析器（处理半包、坏包）
 * - 辅助工具
 */

#include "bytes.hpp"
#include <optional>
#include <expected>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <algorithm>

namespace erpc {

    // ============================================================================
    // 1. 常量与工具
    // ============================================================================

    constexpr char MAGIC[] = "EMSG";
    constexpr size_t FRAGMENT_HEADER_SIZE = 20;
    constexpr size_t DEFAULT_FRAGMENT_PAYLOAD_SIZE = 8192;
    constexpr size_t MAX_MESSAGE_SIZE = 64 * 1024 * 1024; // 64 MB

    inline std::optional<size_t> ParseDecimalString(const char* str, size_t len) {
        if (len == 0) return std::nullopt;
        size_t value = 0;
        for (size_t i = 0; i < len; ++i) {
            char c = str[i];
            if (c < '0' || c > '9') return std::nullopt;
            if (value > (SIZE_MAX - (c - '0')) / 10) return std::nullopt;
            value = value * 10 + (c - '0');
            if (value > MAX_MESSAGE_SIZE) return std::nullopt;
        }
        return value;
    }

    // ============================================================================
    // 2. 数据结构
    // ============================================================================

    struct Fragment {
        erpc_imp::bytes data;
    };

    struct LogicalMessage {
        std::string msgId;
        erpc_imp::bytes payload;
        bool IsControlMessage() const {
            return msgId.size() == 4 && msgId >= "0000" && msgId <= "0009";
        }
    };

    enum class BuilderError {
        InvalidMsgId,
        MessageTooLarge
    };

    template <typename T>
    using BuilderResult = std::expected<T, BuilderError>;

    // ============================================================================
    // 3. 构造器
    // ============================================================================

    class ERpcBuilder {
    public:
        static BuilderResult<std::vector<Fragment>> BuildFragments(
            const std::string& msgId,
            const void* payload,
            size_t payloadLen,
            size_t fragSize = DEFAULT_FRAGMENT_PAYLOAD_SIZE)
        {
            if (msgId.size() != 4 || !std::all_of(msgId.begin(), msgId.end(), ::isdigit)) {
                return std::unexpected(BuilderError::InvalidMsgId);
            }

            
            std::string header = "<Msg" + msgId + ">" + std::to_string(payloadLen) + "</Msg" + msgId + ">";
            erpc_imp::bytes logicalFrame;
            logicalFrame
                .append(header.data(), header.size())
                .append(payload, payloadLen);

            if (logicalFrame.size() > MAX_MESSAGE_SIZE) {
                return std::unexpected(BuilderError::MessageTooLarge);
            }

            uint32_t totalLen = static_cast<uint32_t>(logicalFrame.size());
            uint32_t seq = 0;
            size_t offset = 0;
            size_t remaining = logicalFrame.size();

            std::vector<Fragment> fragments;
            while (remaining > 0) {
                size_t chunkSize = std::min(remaining, fragSize);
                uint32_t pktLen = static_cast<uint32_t>(chunkSize);

                
                Fragment frag;
                frag.data = erpc_imp::bytes()
                    .append(MAGIC, 4)                                                         // 4 字节 Magic
                    .append(msgId.c_str(), 4)                                                // 4 字节 MsgId
                    .append(to_bytes(totalLen, erpc_imp::bytes::Endianness::LittleEndian))   // 4 字节 TotalLen
                    .append(to_bytes(seq, erpc_imp::bytes::Endianness::LittleEndian))        // 4 字节 Seq
                    .append(to_bytes(pktLen, erpc_imp::bytes::Endianness::LittleEndian))     // 4 字节 PktLen
                    .append(logicalFrame.data() + offset, chunkSize);                        // 载荷

                fragments.push_back(std::move(frag));
                offset += chunkSize;
                remaining -= chunkSize;
                ++seq;
            }
            return fragments;
        }

        static BuilderResult<std::vector<Fragment>> BuildFragments(
            const std::string& msgId,
            const std::string& payload,
            size_t fragSize = DEFAULT_FRAGMENT_PAYLOAD_SIZE)
        {
            return BuildFragments(
                msgId,
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size(),
                fragSize
            );
        }
    };

    // ============================================================================
    // 4. 解析器（使用 extract_num、find、slice）
    // ============================================================================

    class ERpcParser {
    public:
        std::optional<LogicalMessage> Feed(const uint8_t* data, size_t len) {
            buffer_.append(data, len);
            return TryParse();
        }

        std::optional<LogicalMessage> Feed(const erpc_imp::bytes& data) {
            buffer_.append(data);
            return TryParse();
        }

        void Reset() {
            buffer_.clear();
            reassembly_.clear();
        }

    private:
        struct ReassemblyContext {
            uint32_t totalLen = 0;
            uint32_t nextSeq = 0;
            erpc_imp::bytes data;
            bool completed = false;

            bool AddFragment(uint32_t seq, const erpc_imp::bytes& payload) {
                if (seq != nextSeq) {
                    data.clear();
                    nextSeq = seq;
                    totalLen = 0;
                }
                if (data.empty()) {
                    if (totalLen > MAX_MESSAGE_SIZE) {
                        completed = false;
                        return false;
                    }
                    data.reserve(totalLen);
                }
                data.append(payload);
                nextSeq = seq + 1;
                if (data.size() >= totalLen) {
                    completed = true;
                }
                return true;
            }
        };

        std::optional<LogicalMessage> TryParse() {
            using erpc_imp::bytes;

            const bytes magicBytes(MAGIC, 4);
            size_t offset = 0;

            while (offset + 4 <= buffer_.size()) {
                auto pos = buffer_.find(magicBytes, offset);
                if (!pos.has_value()) {
                    if (buffer_.size() > 3) {
                        buffer_ = buffer_.slice(buffer_.size() - 3).to_bytes();
                    }
                    break;
                }

                size_t magicPos = pos.value();
                if (magicPos > 0) {
                    buffer_ = buffer_.slice(magicPos).to_bytes();
                    magicPos = 0;
                }

                if (buffer_.size() < FRAGMENT_HEADER_SIZE) {
                    break;
                }

                
                auto totalLenOpt = buffer_.extract_num<uint32_t>(8, bytes::Endianness::LittleEndian);
                auto seqOpt = buffer_.extract_num<uint32_t>(12, bytes::Endianness::LittleEndian);
                auto pktLenOpt = buffer_.extract_num<uint32_t>(16, bytes::Endianness::LittleEndian);
                if (!totalLenOpt || !seqOpt || !pktLenOpt) {
                    buffer_ = buffer_.slice(FRAGMENT_HEADER_SIZE).to_bytes();
                    continue;
                }
                uint32_t totalLen = *totalLenOpt;
                uint32_t seq = *seqOpt;
                uint32_t pktLen = *pktLenOpt;

                if (totalLen > MAX_MESSAGE_SIZE || pktLen > totalLen) {
                    buffer_ = buffer_.slice(FRAGMENT_HEADER_SIZE).to_bytes();
                    continue;
                }

                size_t needed = FRAGMENT_HEADER_SIZE + pktLen;
                if (needed < FRAGMENT_HEADER_SIZE || buffer_.size() < needed) {
                    break;
                }

                std::string msgId(reinterpret_cast<const char*>(buffer_.data() + 4), 4);
                bytes payload = buffer_.slice(FRAGMENT_HEADER_SIZE, pktLen).to_bytes();

                buffer_ = buffer_.slice(needed).to_bytes();

                auto& ctx = reassembly_[msgId];
                if (seq == 0) {
                    ctx.totalLen = totalLen;
                    ctx.nextSeq = 0;
                    ctx.data.clear();
                    ctx.completed = false;
                }
                if (!ctx.AddFragment(seq, payload)) {
                    reassembly_.erase(msgId);
                    continue;
                }

                if (ctx.completed) {
                    auto result = ExtractLogicalMessage(msgId, std::move(ctx.data));
                    reassembly_.erase(msgId);
                    if (result) return result;
                }
            }
            return std::nullopt;
        }

        std::optional<LogicalMessage> ExtractLogicalMessage(
            const std::string& msgId,
            erpc_imp::bytes&& rawLogicalFrame)
        {
            using erpc_imp::bytes;

            auto startTagEnd = rawLogicalFrame.find(bytes(">", 1));
            if (!startTagEnd.has_value()) return std::nullopt;
            size_t tagEndPos = startTagEnd.value();

            std::string endTag = "</Msg" + msgId + ">";
            bytes endTagBytes(endTag.data(), endTag.size());
            auto endPos = rawLogicalFrame.find(endTagBytes, tagEndPos);
            if (!endPos.has_value()) return std::nullopt;

            size_t payloadStart = endPos.value() + endTag.size();
            bytes payload;
            if (payloadStart < rawLogicalFrame.size()) {
                payload = rawLogicalFrame.slice(payloadStart).to_bytes();
            }

            LogicalMessage msg;
            msg.msgId = msgId;
            msg.payload = std::move(payload);
            return msg;
        }

        erpc_imp::bytes buffer_;
        std::unordered_map<std::string, ReassemblyContext> reassembly_;
    };

    // ============================================================================
    // 5. 辅助工具
    // ============================================================================

    inline erpc_imp::bytes BuildLogicalFrame(
        const std::string& msgId,
        const uint8_t* payload,
        size_t payloadLen)
    {
        std::string header = "<Msg" + msgId + ">" + std::to_string(payloadLen) + "</Msg" + msgId + ">";
        erpc_imp::bytes frame;
        frame
            .append(header.data(), header.size())
            .append(payload, payloadLen);
        return frame;
    }

    inline std::optional<std::pair<std::string, erpc_imp::bytes>> ParseLogicalFrame(
        const uint8_t* data,
        size_t len)
    {
        using erpc_imp::bytes;
        if (len < 10) return std::nullopt;

        bytes frame(data, len);
        auto startPos = frame.find(bytes("<", 1));
        if (!startPos.has_value()) return std::nullopt;
        size_t start = startPos.value();

        auto endPos = frame.find(bytes(">", 1), start);
        if (!endPos.has_value()) return std::nullopt;
        size_t end = endPos.value();
        if (end - start < 10) return std::nullopt;

        std::string msgId(reinterpret_cast<const char*>(frame.data() + start + 4), 4);
        if (msgId.size() != 4 || !std::all_of(msgId.begin(), msgId.end(), ::isdigit)) {
            return std::nullopt;
        }

        std::string endTag = "</Msg" + msgId + ">";
        auto tagPos = frame.find(bytes(endTag.data(), endTag.size()), end);
        if (!tagPos.has_value()) return std::nullopt;
        size_t tagEnd = tagPos.value();

        size_t lenStart = start + 9;
        size_t lenEnd = tagEnd;
        if (lenEnd <= lenStart) return std::nullopt;
        auto lenOpt = ParseDecimalString(
            reinterpret_cast<const char*>(frame.data() + lenStart),
            lenEnd - lenStart);
        if (!lenOpt) return std::nullopt;
        size_t payloadLen = *lenOpt;

        size_t payloadStart = tagEnd + endTag.size();
        if (payloadStart > len) return std::nullopt;

        bytes payload;
        if (payloadLen > 0) {
            if (payloadStart + payloadLen > len) return std::nullopt;
            payload = frame.slice(payloadStart, payloadLen).to_bytes();
        }
        return std::make_pair(msgId, std::move(payload));
    }

} // namespace erpc

#endif // EPRC_PARSE_BYTES_HPP