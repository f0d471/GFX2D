// v1-I 坐标取数：单个 256×32 同步缓存，最多接收 64 个 128 位拍。
// 每个 DMA 拍按四个整字顺序写入；tready 反压由读核 FIFO 吸收。
// command/instance 由搬运层保存的 owner 明确分流；不进入命令 cbuf。
// 每个 entry 在 ready 前保持，包内按画序推进；坏突发退到单拍定位首错。
`timescale 1ns/1ps
`default_nettype none
module engine_inst (
    input wire clk,rst,cancel,go,
    input wire [31:0] base,
    input wire [15:0] count,
    input wire [8:0] stride,
    input wire layout,
    input wire [31:0] fixed_uv,
    input wire [3:0] context_id,
    output wire busy,
    output wire [31:0] fd_addr,
    output wire [11:0] fd_bytes,
    output wire [3:0] fd_cidx,
    output wire fd_valid,
    input wire fd_ready,
    input wire [127:0] fs_data,
    input wire fs_valid,fs_last,
    input wire [3:0] fs_cidx,
    output wire fs_ready,
    input wire stat_valid,stat_error,
    input wire [3:0] stat_cidx,
    output wire valid,
    input wire ready,
    output reg [31:0] xy,uv,
    output reg [31:0] instance_id,
    output reg error,
    output reg [3:0] error_code
);
    localparam IDLE=0,LOOK=1,REQ=2,RECV=3,RD0=4,RD1=5,WORD=6,OUT=7,FAIL=8;
    reg [3:0] state;
    reg [31:0] cursor,word_addr,cache_base,req_base;
    reg [32:0] array_end,cache_end;
    reg [15:0] n;
    reg [8:0] step;
    reg kind,second,cache_valid,single_mode,canceled;
    reg [31:0] fixed_r;
    reg [3:0] ctx;
    reg [6:0] beats,received;
    reg status_seen,status_bad,last_seen,protocol_bad;
    reg [31:0] cache[0:255];
    reg [31:0] cache_q;
    reg [127:0] store_data;
    reg [5:0] store_beat;
    reg [1:0] store_lane;
    reg storing;
    wire fire=fs_valid && fs_ready;
    wire [31:0] offset=word_addr-cache_base;
    wire [7:0] row=offset[9:2];
    wire [31:0] word_value=cache_q;
    always @(posedge clk) begin
        if(storing)cache[{store_beat,store_lane}]<=store_data[31:0];
        cache_q<=cache[row];
    end
    always @(posedge clk) begin
        if(rst)storing<=0;
        else if(fire)begin
            store_data<=fs_data;store_beat<=received[5:0];store_lane<=0;storing<=1;
        end else if(storing)begin
            store_data<={32'd0,store_data[127:32]};store_lane<=store_lane+2'd1;
            if(store_lane==3)storing<=0;
        end
    end
    wire [15:0] count_minus=count-16'd1;
    wire [24:0] decoded_length=count_minus*stride+(layout?25'd8:25'd4);
    wire [31:0] aligned={word_addr[31:4],4'd0};
    wire [32:0] remain=array_end-{1'b0,aligned};
    wire [8:0] page_beats=9'd256-{1'b0,aligned[11:4]};
    wire [32:0] total_beats=(remain+33'd15)>>4;
    wire [6:0] cap_beats=single_mode?7'd1:7'd64;
    wire [6:0] page_cap=page_beats>64?7'd64:page_beats[6:0];
    wire [6:0] cap1=page_cap<cap_beats?page_cap:cap_beats;
    wire [6:0] next_beats=total_beats<{26'd0,cap1}?total_beats[6:0]:cap1;
    assign busy=state!=IDLE;
    assign fd_addr=req_base;
    assign fd_bytes={1'b0,beats,4'b0};
    assign fd_cidx=ctx;
    assign fd_valid=state==REQ && !cancel;
    assign fs_ready=state==RECV && !storing;
    assign valid=state==OUT && !cancel;
    wire status_in=stat_valid;
    wire finish=(last_seen || (fire && fs_last)) && (status_seen || status_in);
    wire [7:0] final_received={1'b0,received}+(fire?8'd1:8'd0);
    wire bad_protocol=protocol_bad || (fire && ((fs_last!=(received+7'd1==beats)) || fs_cidx!=ctx)) ||
                      (status_in && stat_cidx!=ctx) || final_received!={1'b0,beats};
    wire bad_bus=status_bad || (status_in && stat_error);
    always @(posedge clk) begin
        if(rst) begin
            state<=IDLE;error<=0;error_code<=0;instance_id<=0;cache_valid<=0;
            canceled<=0;status_seen<=0;last_seen<=0;protocol_bad<=0;
        end else begin
            if(cancel && state==RECV)canceled<=1;
            if(cancel && state!=RECV) begin state<=IDLE;error<=0;end
            else case(state)
                IDLE: if(go) begin
                    cursor<=base;word_addr<=base;n<=count;step<=stride;kind<=layout;
                    fixed_r<=fixed_uv;ctx<=context_id;instance_id<=0;error<=0;error_code<=0;
                    array_end<={1'b0,base}+((count==0)?33'd0:{8'd0,decoded_length});
                    cache_valid<=0;single_mode<=0;canceled<=0;second<=0;
                    state<=count==0?IDLE:LOOK;
                end
                LOOK: begin
                    if(cache_valid && word_addr>=cache_base && {1'b0,word_addr}+33'd4<=cache_end)
                        state<=RD0;
                    else begin req_base<=aligned;beats<=next_beats;state<=REQ;end
                end
                REQ: if(fd_ready) begin
                    received<=0;status_seen<=0;status_bad<=0;last_seen<=0;protocol_bad<=0;
                    cache_valid<=0;state<=RECV;
                end
                RECV: begin
                    if(fire) begin
                        received<=received+7'd1;
                        if(fs_last)last_seen<=1;
                        if(fs_last!=(received+7'd1==beats) || fs_cidx!=ctx)protocol_bad<=1;
                    end
                    if(status_in) begin
                        status_seen<=1;status_bad<=stat_error;
                        if(stat_cidx!=ctx)protocol_bad<=1;
                    end
                    if(finish && !storing && !fire) begin
                        if(canceled || cancel)begin state<=IDLE;error<=0;end
                        else if(bad_protocol)begin error<=1;error_code<=9;state<=FAIL;end
                        else if(bad_bus && !single_mode)begin single_mode<=1;state<=LOOK;end
                        else if(bad_bus)begin error<=1;error_code<=10;state<=FAIL;end
                        else begin
                            cache_base<=req_base;cache_end<={1'b0,req_base}+{22'd0,beats,4'd0};
                            cache_valid<=1;state<=RD0;
                        end
                    end
                end
                RD0: state<=RD1;
                RD1: state<=WORD;
                WORD: begin
                    if(!second) begin
                        xy<=word_value;
                        if(kind)begin second<=1;word_addr<=cursor+32'd4;state<=LOOK;end
                        else begin uv<=fixed_r;state<=OUT;end
                    end else begin uv<=word_value;state<=OUT;end
                end
                OUT: if(ready) begin
                    if(instance_id+32'd1=={16'd0,n})state<=IDLE;
                    else begin
`ifdef INJ_INST_SKIP
                        instance_id<=instance_id+32'd2;
`else
                        instance_id<=instance_id+32'd1;
`endif
`ifdef INJ_INST_STRIDE
                        cursor<=cursor+32'd4;word_addr<=cursor+32'd4;
`else
                        cursor<=cursor+{23'd0,step};word_addr<=cursor+{23'd0,step};
`endif
                        second<=0;state<=LOOK;
                    end
                end
                FAIL: if(cancel)begin state<=IDLE;error<=0;end
                default:state<=IDLE;
            endcase
        end
    end
endmodule
`default_nettype wire
