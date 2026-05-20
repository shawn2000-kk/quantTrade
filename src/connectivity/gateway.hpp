#pragma once

#include <cstdint>
#include <string_view>

#include "oms/order.hpp"  // OrderRequest, ModifyRequest

// ============================================================
// gateway.hpp — IGateway 统一网关接口（纯虚基类）
//
// 所有具体网关（FIX、Binary Protocol、SimGateway）都继承此接口，
// 执行引擎通过 IGateway* 操作，不依赖具体协议实现。
// 热路径方法全部标记 noexcept，配合 -fno-exceptions 编译。
// ============================================================

namespace hft {

class IGateway {
public:
    virtual ~IGateway() = default;

    // ── 报单操作（热路径，noexcept）──────────────────────────────

    /// 发送新报单。底层实现负责将 OrderRequest 编码并通过网络发出。
    virtual void send_new_order(const OrderRequest& req) noexcept = 0;

    /// 发送撤单请求。使用 OMS 生成的 client_order_id 标识目标订单。
    virtual void send_cancel(uint64_t client_order_id) noexcept = 0;

    /// 发送改单请求（价格 / 数量修改）。
    virtual void send_modify(const ModifyRequest& req) noexcept = 0;

    // ── 连接状态查询 ──────────────────────────────────────────────

    /// 返回当前网关是否处于已连接可用状态（被 SessionManager 心跳线程轮询）。
    [[nodiscard]] virtual bool is_connected() const noexcept = 0;

    /// 返回最近一次报单往返延迟（纳秒）。0 表示尚无测量数据。
    [[nodiscard]] virtual uint64_t get_rtt_ns() const noexcept = 0;

    /// 网关名称，用于日志 / 监控标签。返回值生命周期与对象相同。
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

} // namespace hft
