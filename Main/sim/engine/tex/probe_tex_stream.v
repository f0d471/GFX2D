// 研究探针：恒 ready 消费者的同步读流，尚非入库 TEX 实现。
// 独立模型提供整帧源数据；LATENCY 是请求到流可采样之间的寄存级数。
// 不包含 APB、16 KiB 边界、命令预取仲裁或复位排空，不能替代整机验收。
`timescale 1ns/1ps
`default_nettype none
module probe_tex_stream #(
    parameter integer LATENCY = 1,
    parameter integer INJECT = 0
)(
    input wire clk, rst,
    input wire [31:0] addr,
    input wire [11:0] beats,
    input wire [1:0] buf_id,
    input wire [3:0] cidx,
    input wire valid,
    output wire ready,
    output reg busy,
    output wire [127:0] data,
    output wire last, stream_valid,
    output wire [1:0] stream_buf,
    output reg status_valid,
    output wire [3:0] status_cidx
);
    reg [127:0] mem [0:32767];
    reg [31:0] read_addr;
    reg [11:0] left;
    reg [1:0] ctx_buf;
    reg [3:0] ctx_cidx;
    reg [127:0] q [0:LATENCY-1];
    reg [LATENCY-1:0] v, l;
    reg tail_seen;
    integer i;
    wire issue = busy && left != 0;
    assign ready = !busy;
    assign data = q[LATENCY-1];
    assign stream_valid = (INJECT == 1) ? issue : v[LATENCY-1];
    assign last = l[LATENCY-1];
    assign stream_buf = ctx_buf;
    assign status_cidx = ctx_cidx;
    always @(posedge clk) begin
        if (rst) begin
            busy <= 0; left <= 0; v <= 0; l <= 0;
            status_valid <= 0; tail_seen <= 0;
            read_addr <= 0; ctx_buf <= 0; ctx_cidx <= 0;
        end else begin
            status_valid <= 0;
            v[0] <= issue; l[0] <= issue && left == 1;
            if (issue) begin
                q[0] <= mem[read_addr[18:4]];
                read_addr <= read_addr + 16;
                left <= left - 1;
            end
            for (i=1; i<LATENCY; i=i+1) begin
                v[i] <= v[i-1]; l[i] <= l[i-1]; q[i] <= q[i-1];
            end
            if (valid && ready) begin
                busy <= 1; left <= beats; read_addr <= addr;
                ctx_buf <= buf_id; ctx_cidx <= cidx; tail_seen <= 0;
            end
            if (v[LATENCY-1] && l[LATENCY-1]) begin
                status_valid <= 1; tail_seen <= 1;
            end
            // 保留到状态被消费者看见，禁止下一笔覆盖上一笔的上下文。
            if (tail_seen) begin busy <= 0; tail_seen <= 0; end
        end
    end
endmodule
`default_nettype wire
