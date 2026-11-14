`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_bin.v
//
// DESCRIPTION: PB 两遍构建：第一遍 COUNT/SNAPSHOT 计数并写 metadata，PREFIX 顺序
//   排他求目录，第二遍 SCATTER 按输入序散写引用，收尾补奇引用 padding 与 32 项
//   桶目录，全部 B 响应与对账通过后发布 READY。
//
// NOTE:
//   1. 片上只保存 32 桶的计数/游标/半引用（32×96 位 RAM）；实例仍由 engine_inst 流式读取
//   2. 私有 clip 与旧直通状态无关，输入处理结束不回写；SOFT_RST 由上层经 rst 覆盖
//   3. count/offset/cursor 压缩保存但外部目录/PERF 为 32 位，标量哨兵无损恢复
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"
module engine_bin #(parameter integer STRIP_H = 16) (
    input  wire         clk,
    input  wire         rst,
    input  wire         go,
    input  wire [31:0]  input_base,
    input  wire [31:0]  input_count,
    input  wire [31:0]  pb_base,
    input  wire [31:0]  pb_bytes,
    input  wire [31:0]  draw_cap,
    input  wire [31:0]  ref_cap,
    input  wire [5:0]   strips,
    input  wire [3:0]   target,
    input  wire [31:0]  bg_base,
    input  wire [31:0]  bg_bytes,
    output wire         busy,
    output reg          done,
    output reg          failed,
    output reg [3:0]    error_code,
    output reg [3:0]    error_stage,
    output reg [31:0]   error_index,
    output reg [31:0]   error_instance,
    output reg [31:0]   error_tag,
    output wire [31:0]  active_index,
    output wire [31:0]  active_instance,
    output wire [31:0]  active_tag,
    output wire [1:0]   write_context,
    output reg          perf_cmd,
    output reg          perf_instance,
    output reg [1:0]    ref_add,
    output reg [31:0]   packets,
    output reg [31:0]   total_refs,
    output reg [31:0]   reference_offset,
    input  wire [4:0]   directory_id,
    output wire [31:0]  directory_count,
    output wire [31:0]  directory_offset,
    output wire [3:0]   src_id,
    output wire [3:0]   dst_id,
    input  wire [31:0]  src_base,
    input  wire [31:0]  src_dim,
    input  wire [31:0]  src_ctrl,
    input  wire [31:0]  dst_base,
    input  wire [31:0]  dst_dim,
    input  wire [31:0]  dst_ctrl,
    input  wire [255:0] play_packet,
    output wire [3:0]   play_error,
    output reg          request,
    output reg          write,
    output reg [31:0]   address,
    output reg [6:0]    bytes,
    output wire [511:0] write_data,
    input  wire         io_ready,
    input  wire         io_done,
    input  wire         io_error,
    input  wire [127:0] read_beat,
    input  wire [1:0]   read_index,
    input  wire         read_valid,
    output wire [31:0]  inst_fd_addr,
    output wire [11:0]  inst_fd_bytes,
    output wire [3:0]   inst_fd_cidx,
    output wire         inst_fd_valid,
    input  wire         inst_fd_ready,
    input  wire [127:0] fs_data,
    input  wire         fs_valid,
    input  wire         fs_last,
    input  wire [3:0]   fs_cidx,
    output wire         inst_fs_ready,
    input  wire         inst_status,
    input  wire         inst_status_error,
    input  wire [3:0]   inst_status_cidx
);
    localparam IDLE=0,INIT=1,GET=2,CHECK=3,META=4,PREP=5,ITEM=6,BOUND=7,
        BUCKET=8,NEXT_ITEM=9,NEXT=10,PREFIX=11,TAIL=12,HEADER=13,WAIT=14,
        GOT=15,WROTE=16,FINISH=17,FAIL=18,SCATTER=19,COUNT_REF=20,
        GOT_META=21,TAIL_DONE=22,HEADER_DONE=23,RAM_WAIT=24,CLEAR=25,
        HEADER_READ=26,HEADER_WRITE=27,CHECK_LATCH=28,CHECK_INPUT=29,HEADER_READ2=30;
    reg [4:0] state,ret;
    reg second_pass;
    reg busy_r;
    reg [15:0] index_r,instance_r;
    wire [31:0] index={16'd0,index_r};
    wire [31:0] instance_id=instance_r==16'hffff ? 32'hffffffff : {16'd0,instance_r};
    reg [255:0] packet;
    reg [63:0] clip;
    wire [63:0] snapshot=clip;
    reg [31:0] xy,uv;
    reg [4:0] bucket,last_bucket,scan;
    reg [4:0] header_chunk;
    // count 18 位、offset 21 位、cursor 18 位、半引用 32 位，共 89 位。
    // packet<32768、instance<65535，压缩身份可无损恢复标量哨兵。
    reg [95:0] bucket_ram[0:31];
    reg [95:0] bucket_q,bucket_wdata;
    reg [4:0] port_id,ram_ret;
    reg bucket_we;
    wire [4:0] bucket_addr=state==IDLE?directory_id:port_id;
    wire [31:0] ram_count={14'd0,bucket_q[17:0]};
    wire [31:0] ram_offset={11'd0,bucket_q[38:18]};
    wire [31:0] ram_cursor={14'd0,bucket_q[56:39]};
    wire [63:0] ram_half={bucket_q[88:73]==16'hffff ? 32'hffffffff : {16'd0,bucket_q[88:73]},16'd0,bucket_q[72:57]};
    reg [127:0] payload;
    reg payload_meta;
    assign write_data=payload_meta?{160'd0,index,snapshot,packet}:{384'd0,payload};

    reg [20:0] prefix;
    reg [17:0] scattered;
    reg inst_go;
    reg frame_seen;
    wire inst_busy,inst_valid,inst_error;
    wire [31:0] inst_xy,inst_uv,inst_id;
    wire [3:0] inst_error_code;
    wire [3:0] checked_error;
    reg [3:0] checked_error_q;
    reg [31:0] check_src_base,check_src_dim,check_dst_base,check_dst_dim;
    reg [12:0] check_src_code,check_dst_code;
    wire [31:0] check_src_ctrl={check_src_code[12],15'd0,check_src_code[11:8],2'd0,check_src_code[7:6],2'd0,check_src_code[5],check_src_code[4:0]};
    wire [31:0] check_dst_ctrl={check_dst_code[12],15'd0,check_dst_code[11:8],2'd0,check_dst_code[7:6],2'd0,check_dst_code[5],check_dst_code[4:0]};
    wire bounds_empty;
    wire [5:0] first_strip,last_strip;
    assign busy=busy_r;
    assign active_index=index;
    assign active_instance=(state==WAIT&&write&&ret==WROTE)?instance_id:
        packet[7:0]==5?inst_id:32'hffffffff;
    assign active_tag=packet[255:224];
    assign write_context=(state==WAIT&&write)?(ret==WROTE?2'd2:ret==PREP?2'd1:2'd0):2'd0;
    assign directory_count=ram_count;
    assign directory_offset=ram_offset;
    assign src_id=packet[19:16];
    assign dst_id=packet[23:20];
    wire [37:0] input_size={1'b0,input_count,5'd0};
    assign play_error=checked_error;
    // 在包选择和表面读口之后锁存，分开 RAM/MUX 与结构/范围校验的组合路径。
    always @(posedge clk)begin
        check_src_base<=src_base;check_src_dim<=src_dim;
        check_dst_base<=dst_base;check_dst_dim<=dst_dim;
        check_src_code<={src_ctrl[31],src_ctrl[15:12],src_ctrl[9:8],|(src_ctrl&32'h7fff0ce0),src_ctrl[4:0]};
        check_dst_code<={dst_ctrl[31],dst_ctrl[15:12],dst_ctrl[9:8],|(dst_ctrl&32'h7fff0ce0),dst_ctrl[4:0]};
    end
    engine_packet_check u_check(.clk(clk),.packet(busy?packet:play_packet),.src_base(check_src_base),.src_dim(check_src_dim),.src_ctrl(check_src_ctrl),
        .dst_base(check_dst_base),.dst_dim(check_dst_dim),.dst_ctrl(check_dst_ctrl),.target(target),
        .pb_base(pb_base),.pb_bytes(pb_bytes),.input_base(input_base),.input_bytes(input_size[31:0]),
        .bg_base(bg_base),.bg_bytes(bg_bytes),.error(checked_error));
    engine_bounds #(.STRIP_H(STRIP_H)) u_bounds(.packet(packet),.xy(xy),.uv(uv),
        .clip(snapshot),.src_dim(src_dim),.dst_dim(dst_dim),
        .empty(bounds_empty),.first_strip(first_strip),.last_strip(last_strip));
    engine_inst u_inst(.clk(clk),.rst(rst),.cancel(state==FAIL),.go(inst_go),
        .base(packet[63:32]),.count(packet[79:64]),.stride(packet[168:160]),
        .layout(packet[176]),.fixed_uv(packet[223:192]),.context_id(index[3:0]),
        .busy(inst_busy),.fd_addr(inst_fd_addr),.fd_bytes(inst_fd_bytes),.fd_cidx(inst_fd_cidx),
        .fd_valid(inst_fd_valid),.fd_ready(inst_fd_ready),.fs_data(fs_data),.fs_valid(fs_valid),
        .fs_last(fs_last),.fs_cidx(fs_cidx),.fs_ready(inst_fs_ready),
        .stat_valid(inst_status),.stat_error(inst_status_error),.stat_cidx(inst_status_cidx),
        .valid(inst_valid),.ready(state==ITEM),.xy(inst_xy),.uv(inst_uv),.instance_id(inst_id),
        .error(inst_error),.error_code(inst_error_code));
    wire signed [17:0] cx=$signed({{2{packet[47]}},packet[47:32]});
    wire signed [17:0] cy=$signed({{2{packet[63]}},packet[63:48]});
    wire signed [17:0] ex=cx+$signed({2'd0,packet[111:96]})-18'sd1;
    wire signed [17:0] ey=cy+$signed({2'd0,packet[127:112]})-18'sd1;
    wire signed [17:0] nx=cx<0?18'sd0:cx, ny=cy<0?18'sd0:cy;
    wire signed [17:0] xx=ex>639?18'sd639:ex, yy=ey>479?18'sd479:ey;
    wire [63:0] next_clip=(packet[111:96]==0 || packet[127:112]==0 || nx>xx || ny>yy)
        ?64'hffffffff00000000:{yy[15:0],xx[15:0],ny[15:0],nx[15:0]};
    wire [63:0] current_ref={packet[7:0]==5?instance_id:32'hffffffff,index};
    localparam [31:0] INIT_STATUS=1;
    wire [32:0] next_prefix={12'd0,prefix}+((({1'b0,ram_count}<<3)+33'd15)&~33'd15);
    wire [32:0] next_sum={1'b0,total_refs}+{1'b0,ram_count};
    wire [127:0] header_data=header_chunk==0?
        {{26'd0,strips},32'd2,32'd1,32'h31425046}:
        header_chunk==1?{reference_offset,32'd512,total_refs,packets}:128'd0;
    integer j;
    always @* begin
        bucket_we=0;bucket_wdata=bucket_q;
        if(state==CLEAR)begin bucket_we=1;bucket_wdata=0;end
        if(state==BUCKET && !second_pass && ram_count<ref_cap)begin
            bucket_we=1;bucket_wdata[17:0]=bucket_q[17:0]+18'd1;
        end
        if(state==BUCKET && second_pass && !ram_cursor[0])begin
            bucket_we=1;bucket_wdata[56:39]=bucket_q[56:39]+18'd1;
            bucket_wdata[88:57]={packet[7:0]==5?instance_id[15:0]:16'hffff,index[15:0]};
        end
        if(state==WROTE)begin bucket_we=1;bucket_wdata[56:39]=bucket_q[56:39]+18'd1;end
        if(state==PREFIX && scan<strips && next_sum<={1'b0,ref_cap} &&
            next_prefix+{1'b0,reference_offset}<={1'b0,pb_bytes})begin
            bucket_we=1;bucket_wdata[38:18]=prefix[20:0];
        end
    end
    always @(posedge clk)begin
        if(bucket_we)bucket_ram[bucket_addr]<=bucket_wdata;
        bucket_q<=bucket_ram[bucket_addr];
    end
    task read_bucket;
        input [4:0] id,next;
        begin port_id<=id;ram_ret<=next;state<=RAM_WAIT;end
    endtask
    task io;
        input wr;
        input [31:0] a;
        input [6:0] n;
        input [511:0] d;
        input [4:0] next;
        begin request<=1;write<=wr;address<=a;bytes<=n;payload<=d[127:0];
            payload_meta<=wr && next==PREP;ret<=next;state<=WAIT;end
    endtask
    task fault;
        input [3:0] code,stage;
        begin failed<=1;error_code<=code;error_stage<=stage;
            error_index<=index;error_tag<=packet[255:224];
            error_instance<=(packet[7:0]==5 && (stage==2 || stage==3))?instance_id:32'hffffffff;state<=FAIL;end
    endtask
    always @(posedge clk) begin
        if(rst) begin state<=IDLE;busy_r<=0;request<=0;done<=0;failed<=0;inst_go<=0;perf_cmd<=0;perf_instance<=0;ref_add<=0;frame_seen<=0;end
        else begin
            request<=0;done<=0;inst_go<=0;perf_cmd<=0;perf_instance<=0;ref_add<=0;
            // 命令/metadata 直接接收读拍，io_done 只发布已收齐且响应成功的对象。
            if(read_valid && state==WAIT && !write && (ret==GOT || ret==GOT_META))begin
                if(read_index==0)packet[127:0]<=read_beat;
                if(read_index==1)packet[255:128]<=read_beat;
                if(read_index==2 && ret==GOT_META)begin
                    clip<=read_beat[63:0];
                end
            end
            case(state)
                IDLE: if(go)begin
                    busy_r<=1;
                    failed<=0;error_code<=0;error_stage<=0;error_index<=0;error_tag<=0;error_instance<=32'hffffffff;
                    index_r<=0;instance_r<=0;packets<=0;total_refs<=0;scattered<=0;
                    packet<=0;second_pass<=0;clip<=64'h01df027f00000000;
                    reference_offset<=512+((draw_cap+1)&32'hfffffffe)*64;
                    port_id<=0;frame_seen<=1;
                    state<=CLEAR;
                end
                CLEAR:if(port_id==31)state<=INIT;else port_id<=port_id+5'd1;
                RAM_WAIT:state<=ram_ret;
                INIT: if(io_ready)io(1,pb_base,16,{384'd0,{26'd0,strips},INIT_STATUS,32'd1,32'h31425046},GET);
                GET: if(index>=input_count)begin
                    packets<=index;scan<=0;prefix<=0;total_refs<=0;read_bucket(0,PREFIX);end
                     else if(io_ready)io(0,input_base+(index<<5),32,512'd0,GOT);
                GOT:begin instance_r<=0;state<=CHECK_INPUT;end
                CHECK_INPUT:state<=CHECK_LATCH;
                CHECK_LATCH:begin checked_error_q<=checked_error;state<=CHECK;end
                CHECK: if(checked_error_q!=0)fault(checked_error_q,1);
                    else if(packet[7:0]==255)begin
                        packets<=index;scan<=0;prefix<=0;total_refs<=0;read_bucket(0,PREFIX);end
                    else begin
                        if(packet[7:0]==3)clip<=next_clip;
                        state<=META;
                    end
                META: if(io_ready)io(1,pb_base+512+(index<<6),64,{160'd0,index,snapshot,packet},PREP);
                PREP: if(packet[7:0]==0 || packet[7:0]==3)state<=NEXT;
                    else if(packet[7:0]==5)begin
                        if(packet[79:64]==0)state<=NEXT;
                        else begin inst_go<=1;state<=ITEM;end
                    end else begin xy<=packet[95:64];uv<=packet[63:32];instance_r<=16'hffff;state<=BOUND;end
                ITEM: if(inst_error)begin fault(inst_error_code,2);error_instance<=inst_id;end
                    else if(inst_valid)begin
                        xy<=inst_xy;uv<=inst_uv;instance_r<=inst_id[15:0];
                        if(!second_pass)perf_instance<=1;
                        state<=BOUND;
                    end
                BOUND: if(bounds_empty)state<=NEXT_ITEM;
                    else if(last_strip>=strips)fault(2,3);
                    else begin bucket<=first_strip[4:0];last_bucket<=last_strip[4:0];read_bucket(first_strip[4:0],BUCKET);end
                BUCKET: if(!second_pass)begin
                        if(ram_count>=ref_cap)fault(2,3);
                        else state<=COUNT_REF;
                    end else if(!ram_cursor[0])begin
                        scattered<=scattered+18'd1;state<=COUNT_REF;
                    end else if(io_ready)begin
                        io(1,pb_base+reference_offset+ram_offset+((ram_cursor-1)<<3),16,
                            {384'd0,current_ref,ram_half},WROTE);
                    end
                WROTE:begin scattered<=scattered+18'd1;ref_add<=2;state<=COUNT_REF;end
                COUNT_REF: if(bucket==last_bucket)state<=NEXT_ITEM;
                    else begin bucket<=bucket+5'd1;read_bucket(bucket+5'd1,BUCKET);end
                NEXT_ITEM: if(packet[7:0]==5 && instance_id+1<{16'd0,packet[79:64]})state<=ITEM;
                    else state<=NEXT;
                NEXT:begin
                    if(!second_pass)begin perf_cmd<=1;index_r<=index_r+16'd1;state<=GET;end
                    else begin index_r<=index_r+16'd1;state<=SCATTER;end
                end
                PREFIX: if(scan<{1'b0,strips})begin
                    if(next_sum>{1'b0,ref_cap} || next_prefix+{1'b0,reference_offset}>{1'b0,pb_bytes})begin
                        fault(2,3);error_index<=0;error_tag<=0;error_instance<=32'hffffffff;
                    end
                    else begin
                        prefix<=next_prefix[20:0];total_refs<={14'd0,next_sum[17:0]};
                        if({1'b0,scan}+1==strips)begin
                            total_refs<={14'd0,next_sum[17:0]};second_pass<=1;index_r<=0;state<=SCATTER;
                        end else begin scan<=scan+5'd1;read_bucket(scan+5'd1,PREFIX);end
                    end
                end else begin total_refs<=0;second_pass<=1;index_r<=0;state<=SCATTER;end
                SCATTER: if(index==packets)begin scan<=0;read_bucket(0,TAIL);end
                    else if(io_ready)io(0,pb_base+512+(index<<6),64,512'd0,GOT_META);
                // 元数据返回与 COUNT 的输入返回分开，不重新修改私有 clip。
                GOT_META:begin instance_r<=0;state<=PREP;end
                TAIL: if(scan==strips)begin
                    if(scattered!=total_refs)fault(2,3);
                    else begin header_chunk<=1;state<=HEADER;end
                end else if(ram_cursor!=ram_count)fault(2,3);
                    else if(ram_cursor[0] && io_ready)begin
                        io(1,pb_base+reference_offset+ram_offset+((ram_cursor-1)<<3),16,
                            {448'd0,ram_half},TAIL_DONE);
                    end else if(!ram_cursor[0])begin scan<=scan+5'd1;read_bucket(scan+5'd1,TAIL);end
                TAIL_DONE:begin ref_add<=1;scan<=scan+5'd1;read_bucket(scan+5'd1,TAIL);end
                HEADER: if(header_chunk>=4 && header_chunk<=19)begin
                    payload<=0;payload_meta<=0;read_bucket((header_chunk-5'd4)<<1,HEADER_READ);
                end else begin payload<=header_data;payload_meta<=0;state<=HEADER_WRITE;end
                HEADER_READ:begin
                    payload[63:0]<={ram_count,ram_offset};read_bucket(port_id+5'd1,HEADER_READ2);
                end
                HEADER_READ2:begin payload[127:64]<={ram_count,ram_offset};state<=HEADER_WRITE;end
                HEADER_WRITE:if(io_ready)io(1,pb_base+{23'd0,header_chunk,4'd0},16,
                    {384'd0,payload},HEADER_DONE);
                HEADER_DONE:if(header_chunk==0)state<=FINISH;
                    else begin header_chunk<=header_chunk==31?5'd0:header_chunk+5'd1;state<=HEADER;end
                WAIT: if(io_done)begin
                    if(io_error)begin
                        fault(write?4'd11:4'd9,write?4'd4:4'd1);
                        if(!write)begin error_tag<=0;error_instance<=32'hffffffff;end
                        else if(ret==WROTE)error_instance<=packet[7:0]==5?instance_id:32'hffffffff;
                        else if(ret!=PREP)begin error_index<=0;error_tag<=0;error_instance<=32'hffffffff;end
                    end
                    else state<=ret;
                end
                FINISH:begin done<=1;busy_r<=0;state<=IDLE;end
                FAIL: if(!inst_busy)begin done<=1;busy_r<=0;state<=IDLE;end
                default:state<=IDLE;
            endcase
        end
    end
endmodule
`default_nettype wire
