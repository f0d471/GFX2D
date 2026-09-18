// 测试台用 AXI4 从机：128 位数据、INCR 突发，带随机反压与响应延迟
//
// 存储 4096 拍（65536 字节），地址按 (addr >> 4) % 4096 取模。
// 突发长度超过 MAX_LEN 时仍按协议走完整个突发，但回 SLVERR，读回的数据为 0。
//
// 地址通道反压的控制（实验 E5 的 3 端口仿真用，默认值保持原来只有随机反压的行为）：
//   ADDR_STALL_MAX  0 表示不限制；大于 0 时地址通道连续反压不超过这么多拍，
//                   用来给「SoC 地址等待」一个可验证的上界（E5 2.2 节）
//   RD_QUEUE        读事务队列深度，默认 1 保持原有行为（地址通道只在空闲时开）；
//                   大于 1 是流水从机，数据还在流也能收地址，E5 用它复现「下游基本随时可收」
//   WR_QUEUE        写地址队列深度，默认 1 保持原有行为（只在写通道空闲时收 AW）；
//                   大于 1 时写数据还在收也能先收下一个 AW，E5 写方向用它构造与读方向对等的条件
//   hold_aw         高电平期间强制 AWREADY 为低（已授权之后的持续反压）
//   hold_ar         高电平期间强制 ARREADY 为低（同上，读方向）
//
// 注错宏（由 run.sh 传入）：
//   INJECT_FLIP  地址 0x0000_0100 那一拍写入时翻转最低位，读比对必须失配
//   INJECT_HANG  永不发出写响应，主机必须超时
//
// 协议检查：写数据的 WLAST 与 AWLEN 对不上时打印 MODEL: wlast mismatch，并把 wlast_err 加一。
// 测试台必须把 wlast_err 非零判为失败：E5 复核时一个主机替身的 WLAST 错了一拍，
// 这条报警打印了几十次却没有任何用例失败，W 错位后整条写通路死锁，被误读成「CPU 被饿死」。
`timescale 1ns/1ps
`default_nettype none

module axi_ram_model #(
    parameter integer MAX_LEN = 255,
    parameter integer SEED    = 1,     // 每个 ready/valid 拉低的概率约 STALL/8
    parameter integer STALL   = 3,
    parameter integer ADDR_STALL_MAX = 0,
    parameter integer RD_QUEUE = 1,
    parameter integer WR_QUEUE = 1
)(
    input  wire         clk,
    input  wire         rst_n,

    input  wire [31:0]  awaddr,
    input  wire [7:0]   awlen,
    input  wire         awvalid,
    output reg          awready,
    input  wire [127:0] wdata,
    input  wire         wlast,
    input  wire         wvalid,
    output reg          wready,
    output reg  [1:0]   bresp,
    output reg          bvalid,
    input  wire         bready,

    input  wire [31:0]  araddr,
    input  wire [7:0]   arlen,
    input  wire         arvalid,
    output reg          arready,
    output reg  [127:0] rdata,
    output reg  [1:0]   rresp,
    output reg          rlast,
    output reg          rvalid,
    input  wire         rready,

    input  wire         hold_aw,
    input  wire         hold_ar
);

reg [127:0] mem [0:4095];
integer seed_r = SEED;

function stall;
    input dummy;
    begin
        stall = (($random(seed_r) & 7) < STALL);
    end
endfunction

integer i;
initial begin
    for (i = 0; i < 4096; i = i + 1)
        mem[i] = 128'h0;
end

// ---------------- 写 ----------------
localparam W_IDLE = 2'd0, W_DATA = 2'd1, W_RESP = 2'd2;
localparam QDEPTH_W = 16;
reg [1:0]  wst;
reg [31:0] wa;
reg [7:0]  wlen, wcnt;
reg        werr;
reg [15:0] aw_stall_cnt;

// 地址通道连续反压达到上限时强制 ready，给出可验证的上界
wire aw_force_ready = (ADDR_STALL_MAX != 0) && (aw_stall_cnt >= ADDR_STALL_MAX);

// 写地址队列（只在 WR_QUEUE > 1 时使用）：与读方向的 RD_QUEUE 同义，
// 不含正在收数据的那一笔，队列里最多排 WR_QUEUE 笔
reg [31:0] wq_addr [0:QDEPTH_W-1];
reg [7:0]  wq_len  [0:QDEPTH_W-1];
reg [4:0]  wq_head = 5'd0, wq_tail = 5'd0;
wire [4:0] wq_used  = wq_tail - wq_head;
wire       wq_empty = (wq_head == wq_tail);

integer wlast_err = 0;

always @(posedge clk) begin
    if (!rst_n) begin
        wst     <= W_IDLE;
        awready <= 1'b0;
        wready  <= 1'b0;
        bvalid  <= 1'b0;
        bresp   <= 2'b00;
        aw_stall_cnt <= 16'd0;
        wq_head <= 5'd0;
        wq_tail <= 5'd0;
    end else begin
        if (awvalid && awready)     aw_stall_cnt <= 16'd0;
        else if (!awready)          aw_stall_cnt <= aw_stall_cnt + 16'd1;
        else                        aw_stall_cnt <= 16'd0;

        if (WR_QUEUE > 1) begin
            // 流水从机：AW 先入队，写通道空闲时从队头取下一笔
            if (hold_aw || !(wq_used < WR_QUEUE))
                awready <= 1'b0;
            else
                awready <= aw_force_ready ? 1'b1 : !stall(0);
            if (awvalid && awready) begin
                wq_addr[wq_tail[3:0]] <= awaddr;
                wq_len [wq_tail[3:0]] <= awlen;
                wq_tail <= wq_tail + 5'd1;
            end
        end

        case (wst)
        W_IDLE: begin
            if (WR_QUEUE > 1) begin
                if (!wq_empty) begin
                    wa      <= wq_addr[wq_head[3:0]];
                    wlen    <= wq_len [wq_head[3:0]];
                    wcnt    <= 8'd0;
                    werr    <= (wq_len[wq_head[3:0]] > MAX_LEN);
                    wq_head <= wq_head + 5'd1;
                    wst     <= W_DATA;
                end
            end else begin
                awready <= hold_aw ? 1'b0 : (aw_force_ready ? 1'b1 : !stall(0));
                if (awvalid && awready) begin
                    awready <= 1'b0;
                    wa      <= awaddr;
                    wlen    <= awlen;
                    wcnt    <= 8'd0;
                    werr    <= (awlen > MAX_LEN);
                    wst     <= W_DATA;
                end
            end
        end
        W_DATA: begin
            wready <= !stall(0);
            if (wvalid && wready) begin
                if (!werr)
                    mem[(wa >> 4) % 4096] <= wdata
`ifdef INJECT_FLIP
                        ^ ((wa == 32'h0000_0100) ? 128'h1 : 128'h0)
`endif
                        ;
                wa   <= wa + 32'd16;
                wcnt <= wcnt + 8'd1;
                if (wlast != (wcnt == wlen)) begin
                    $display("MODEL: wlast mismatch at beat %0d", wcnt);
                    wlast_err = wlast_err + 1;
                end
                if (wlast) begin
                    wready <= 1'b0;
                    wst    <= W_RESP;
                end
            end
        end
        W_RESP: begin
`ifndef INJECT_HANG
            if (!bvalid && !stall(0)) begin
                bvalid <= 1'b1;
                bresp  <= werr ? 2'b10 : 2'b00;
            end
`endif
            if (bvalid && bready) begin
                bvalid <= 1'b0;
                wst    <= W_IDLE;
            end
        end
        default: wst <= W_IDLE;
        endcase
    end
end

// ---------------- 读 ----------------
// 读事务队列：RD_QUEUE=1 时地址通道只在完全空闲时打开，与本文原有行为一致；
// 大于 1 时是流水从机（数据还在流也能收地址），用来给地址反压一个可验证的小上界
// （E5 的 inject_noguard 用例需要「下游地址通道基本随时可收」才能复现 S2 饿死）。
localparam QDEPTH = 16;
reg [31:0] q_addr [0:QDEPTH-1];
reg [7:0]  q_len  [0:QDEPTH-1];
reg [4:0]  q_head = 5'd0, q_tail = 5'd0;
wire [4:0] q_used  = q_tail - q_head;
wire       q_empty = (q_head == q_tail);
wire       q_room  = (q_used < RD_QUEUE);

reg        srv = 1'b0;
reg [31:0] ra;
reg [7:0]  rlen, rcnt;
reg        rerr;
reg [15:0] ar_stall_cnt;

wire ar_force_ready = (ADDR_STALL_MAX != 0) && (ar_stall_cnt >= ADDR_STALL_MAX);
wire addr_open = q_room && ((RD_QUEUE > 1) || (!srv && q_empty));

always @(posedge clk) begin
    if (!rst_n) begin
        arready <= 1'b0;
        rvalid  <= 1'b0;
        rlast   <= 1'b0;
        rresp   <= 2'b00;
        ar_stall_cnt <= 16'd0;
        q_head  <= 5'd0;
        q_tail  <= 5'd0;
        srv     <= 1'b0;
        ra      <= 32'h0;
        rlen    <= 8'd0;
        rcnt    <= 8'd0;
        rerr    <= 1'b0;
    end else begin
        // 地址通道连续反压计数（ADDR_STALL_MAX 用它给出上界）
        if (arvalid && arready)     ar_stall_cnt <= 16'd0;
        else if (!arready)          ar_stall_cnt <= ar_stall_cnt + 16'd1;
        else                        ar_stall_cnt <= 16'd0;

        // ARREADY：hold 优先，其次看队列有没有空间，最后是随机反压与反压上界
        if (hold_ar || !addr_open)
            arready <= 1'b0;
        else
            arready <= ar_force_ready ? 1'b1 : !stall(0);

        // 入队
        if (arvalid && arready) begin
            q_addr[q_tail[3:0]] <= araddr;
            q_len [q_tail[3:0]] <= arlen;
            q_tail <= q_tail + 5'd1;
        end

        // 取下一笔开始服务
        if (!srv && !q_empty) begin
            srv  <= 1'b1;
            ra   <= q_addr[q_head[3:0]];
            rlen <= q_len [q_head[3:0]];
            rcnt <= 8'd0;
            rerr <= (q_len[q_head[3:0]] > MAX_LEN);
            q_head <= q_head + 5'd1;
        end

        // 数据流
        if (srv) begin
            if (!rvalid || rready) begin
                if (rvalid && rlast) begin
                    rvalid <= 1'b0;
                    srv    <= 1'b0;
                end else if (!stall(0)) begin
                    rvalid <= 1'b1;
                    rdata  <= rerr ? 128'h0 : mem[(ra >> 4) % 4096];
                    rresp  <= rerr ? 2'b10 : 2'b00;
                    rlast  <= (rcnt == rlen);
                    ra     <= ra + 32'd16;
                    rcnt   <= rcnt + 8'd1;
                end else
                    rvalid <= 1'b0;
            end
        end
    end
end

endmodule

`default_nettype wire
