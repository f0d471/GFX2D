`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_pb_dma.v
//
// DESCRIPTION: PB 工作区的小事务桥：16/32/64 B 一笔，读方向逐拍转发给消费者，
//   写方向由生产者持有 payload；等待全部数据与写响应后才发布完成。
//
// NOTE:
//   1. 所有地址由上层验证，本层只查 16 B 对齐、长度与 4 KB 页界
//   2. PB 写走独立 owner 通路，128 位数据原样转发，不做字节抑制
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"

module engine_pb_dma (
    input  wire         clk,
    input  wire         rst,
    input  wire         request,
    input  wire         write,
    input  wire [31:0]  address,
    input  wire [6:0]   bytes,
    input  wire [511:0] write_data,
    // 非构建期（pb 自有 ERROR 头）的窄拍通路：构建器 payload 常驻本口，
    // pb 侧数据走 128 位单拍，避免上游摆 512 位二选一
    input  wire         pb_sel,
    input  wire [127:0] pb_wr_beat,
    output wire         ready,
    output reg          done,
    output reg          error,
    output wire [127:0] read_beat,
    output wire [1:0]   read_index,
    output wire         read_valid,
    output wire [31:0]  fd_addr,
    output wire [11:0]  fd_bytes,
    output wire         fd_valid,
    input  wire         fd_ready,
    input  wire [127:0] fs_data,
    input  wire         fs_valid,
    input  wire         fs_last,
    output wire         fs_ready,
    input  wire         rd_status,
    input  wire         rd_error,
    output wire [31:0]  pw_addr,
    output wire [11:0]  pw_bytes,
    output wire         pw_valid,
    input  wire         pw_ready,
    output wire [127:0] pw_data,
    output wire         pw_data_valid,
    input  wire         pw_data_ready,
    input  wire         pw_status,
    input  wire         pw_error
);

    localparam IDLE = 0, RQ = 1, READ = 2, WQ = 3, WRITE = 4, WAIT = 5, FINISH = 6;

    reg [2:0]  state;
    reg [31:0] addr;
    reg [6:0]  size;
    reg [2:0]  beats, index;
    reg        status_seen, last_seen, bad;

    wire fire = fs_valid && fs_ready;

    // 读通路：描述符发给搬运层读核，读拍原样带通道号转发
    assign ready      = state == IDLE;
    assign fd_addr    = addr;
    assign fd_bytes   = {5'd0, size};
    assign fd_valid   = state == RQ;
    assign fs_ready   = state == READ;
    assign read_beat  = fs_data;
    assign read_index = index[1:0];
    assign read_valid = fire;

    // 写通路：写请求的生产者持有 payload 到 done，桥只选择当前拍，避免重复存 64 B
    assign pw_addr       = addr;
    assign pw_bytes      = {5'd0, size};
    assign pw_valid      = state == WQ;
    assign pw_data       = pb_sel ? pb_wr_beat : write_data[index*128 +: 128];
    assign pw_data_valid = state == WRITE;

    always @(posedge clk) begin
        if (rst) begin
            state <= IDLE;
            done <= 0;
            error <= 0;
        end else begin
            done <= 0;
            case (state)
                IDLE:
                if (request) begin
                    addr <= address;
                    size <= bytes;
                    beats <= bytes[6:4];
                    index <= 0;
                    status_seen <= 0;
                    last_seen <= 0;
                    bad <= 0;
                    error <= 0;
                    if (address[3:0] != 0 || !(bytes == 16 || bytes == 32 || bytes == 64) ||
                        {1'b0, address} + {26'd0, bytes} > 33'h100000000 ||
                        {1'b0, address[11:0]} + {6'd0, bytes} > 4096) begin
                        error <= 1;
                        state <= FINISH;
                    end else begin
                        state <= write ? WQ : RQ;
                    end
                end
                RQ: if (fd_ready) state <= READ;
                READ: begin
                    if (fire) begin
                        index <= index + 3'd1;
                        if (fs_last) last_seen <= 1;
                        if (fs_last != (index + 3'd1 == beats) || index >= beats) bad <= 1;
                    end
                    if (rd_status) begin
                        status_seen <= 1;
                        if (rd_error) bad <= 1;
                    end
                    if ((last_seen || (fire && fs_last)) && (status_seen || rd_status)) begin
                        error <= bad || (rd_status && rd_error) ||
                            (fire && (fs_last != (index + 3'd1 == beats))) ||
                            (index + (fire ? 3'd1 : 3'd0) != beats);
                        state <= FINISH;
                    end
                end
                WQ: if (pw_ready) state <= WRITE;
                WRITE: begin
                    if (pw_data_ready) begin
                        index <= index + 3'd1;
                        if (index + 3'd1 == beats) state <= WAIT;
                    end
                    if (pw_status) begin
                        status_seen <= 1;
                        if (pw_error) bad <= 1;
                    end
                end
                WAIT: if (status_seen || pw_status) begin
                    error <= bad || (pw_status && pw_error);
                    state <= FINISH;
                end
                FINISH: begin
                    done <= 1;
                    state <= IDLE;
                end
                default: state <= IDLE;
            endcase
        end
    end

endmodule
`default_nettype wire
