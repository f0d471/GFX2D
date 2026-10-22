// FD 单元层：独立记录字模式 scoreboard、分片、FIFO、首错与取消排空。
`timescale 1ns/1ps
`default_nettype none
module tb_engine_rlst;
    reg clk=0,rst=1,go=0,cancel=0,ack=0,allow_fd=1;
    always #5 clk=~clk;
    reg [31:0] tri_base=32'h800000,sub_base=32'h100000;
    reg [23:0] sub_bytes=120;
    reg [15:0] tri_count=65535;
    wire [31:0] fd_addr; wire [11:0] fd_bytes; wire [3:0] fd_cidx;
    wire fd_raster,fd_valid,fd_ready,fs_ready;
    reg [127:0] fs_data;
    wire fs_valid,fs_last;
    reg st_valid=0;
    reg [1:0] rp_free=3;
    wire rp_we,rp_valid,strip_req,strip_done,frame_done,error,busy;
    wire [1:0] rp_slot,rp_slot_v;
    wire [4:0] rp_wa,rp_strip,cur_strip;
    wire [127:0] rp_wd;
    wire [15:0] rp_index,rp_ordinal,error_index;
    wire [3:0] error_code; wire [31:0] error_tag;
    reg [7:0] submem[0:4095];
    reg active=0,is_sub=0;
    reg [31:0] address;
    integer left=0,beat=0,mode=0,cycle=0,delivered=0,requests=0,expected_deliver=0;
    integer delay0=0,delay1=0,strip_ends=0,j;
    reg [31:0] slot_words[0:1][0:31];
    wire early_last=mode==3 && beat==0;
    assign fd_ready=allow_fd && !active && !st_valid;
    assign fs_valid=active && cycle%3!=0;
    assign fs_last=mode==4 ? 1'b0 : mode==7 ? left==0 : (left==16 || early_last);
    always @* begin
        fs_data=0;
        for(integer k=0;k<16;k=k+1) begin
            if(is_sub) fs_data[8*k+:8]=submem[(address-sub_base+k)%4096];
        end
        if(!is_sub) for(integer k=0;k<4;k=k+1)
            fs_data[32*k+:32]=(address+k*4)^32'hac130000;
    end
    engine_rlst d(.clk(clk),.rst(rst),.go(go),.cancel(cancel),
        .tri_base(tri_base),.sub_base(sub_base),.sub_bytes(sub_bytes),.tri_count(tri_count),
        .fd_addr(fd_addr),.fd_bytes(fd_bytes),.fd_cidx(fd_cidx),.fd_raster(fd_raster),
        .fd_valid(fd_valid),.fd_ready(fd_ready),.fs_data(fs_data),.fs_valid(fs_valid),
        .fs_last(fs_last),.fs_ready(fs_ready),.fs_keep(mode==1?16'hfffe:16'hffff),
        .fs_cidx(mode==2?4'd1:4'd0),.st_valid(st_valid),.st_error(mode==5),.st_cidx(4'd0),
        .rp_we(rp_we),.rp_slot(rp_slot),.rp_wa(rp_wa),.rp_wd(rp_wd),
        .rp_valid(rp_valid),.rp_slot_v(rp_slot_v),.rp_index(rp_index),.rp_strip(rp_strip),
        .rp_ordinal(rp_ordinal),.rp_free(rp_free),.strip_req(strip_req),.cur_strip(cur_strip),
        .strip_ack(ack),.strip_done(strip_done),.frame_done(frame_done),.error(error),
        .error_code(error_code),.error_index(error_index),.error_tag(error_tag),.busy(busy));
    always @(posedge clk) begin
        cycle<=cycle+1;
        st_valid<=0;
        if(fd_valid && fd_ready) begin
            if(fd_bytes==0 || fd_bytes>128 || fd_addr[3:0]!=0 ||
                {1'b0,fd_addr[11:0]}+fd_bytes>4096 || !fd_raster) $fatal(1,"FD contract");
            active<=1; address<=fd_addr; left<=fd_bytes; beat<=0;
            is_sub<=fd_addr>=sub_base && fd_addr<sub_base+4096;
            requests<=requests+1;
            if(mode==6) st_valid<=1; // 状态先到，流仍必须收齐。
        end
        if(fs_valid && fs_ready) begin
            address<=address+16;left<=left-16;beat<=beat+1;
            if((left==16 && mode!=7) || (left==0 && mode==7) || early_last) begin
                active<=0;
                if(mode!=6) st_valid<=1;
            end
        end
        if(rp_we) for(j=0;j<4;j=j+1) slot_words[rp_slot[0]][rp_wa+j]<=rp_wd[32*j+:32];
        if(rp_valid) begin
            if(!rp_free[rp_slot_v[0]]) $fatal(1,"slot overwrite");
            for(j=0;j<32;j=j+1) if(slot_words[rp_slot_v[0]][j] !==
                ((tri_base+rp_index*128+j*4)^32'hac130000)) $fatal(1,"record reconstruction word=%0d",j);
            delivered<=delivered+1;
            rp_free[rp_slot_v[0]]<=0;
            if(rp_slot_v[0]) delay1<=40+cycle%19; else delay0<=50+cycle%17;
        end
        if(delay0>0) begin delay0<=delay0-1; if(delay0==1) rp_free[0]<=1; end
        if(delay1>0) begin delay1<=delay1-1; if(delay1==1) rp_free[1]<=1; end
        if(strip_req) ack<=1;
        if(strip_done) begin
            if(rp_free!=3 || rp_valid) $fatal(1,"strip before retirement");
            ack<=0;strip_ends<=strip_ends+1;
        end
        if(cycle>1000000) $fatal(1,"RLST WATCHDOG state=%0d",d.state);
    end
    task put16;
        input integer off,value;
        begin submem[off]=value;submem[off+1]=value>>8;end
    endtask
    task prepare;
        input integer count;
        begin
            for(integer k=0;k<4096;k=k+1) submem[k]=0;
            put16(0,count);
            for(integer k=0;k<count;k=k+1) put16(4+2*k,k%2?65534:0);
            sub_bytes=120+2*count;
        end
    endtask
    task run_case;
        input integer wanted_code,wanted_deliver;
        integer before_refs,before_ends;
        begin
            before_refs=delivered;before_ends=strip_ends;
            @(negedge clk);go=1;@(negedge clk);go=0;
            wait(frame_done);@(negedge clk);
            if(error_code!==wanted_code[3:0] || error!==(wanted_code!=0) ||
                delivered-before_refs!=wanted_deliver || active) $fatal(1,"RLST result code=%0d refs=%0d",error_code,delivered-before_refs);
            if(!wanted_code && strip_ends-before_ends!=30) $fatal(1,"empty strip headers");
            $display("CASE PASS mode=%0d base=%h bytes=%0d code=%0d refs=%0d",mode,tri_base,sub_bytes,error_code,wanted_deliver);
        end
    endtask
    task cancel_state;
        input integer target;
        begin
            wait(!busy && rp_free==3 && !active);
            prepare(1);
            @(negedge clk);go=1;@(negedge clk);go=0;
            wait(d.state==target);@(negedge clk);cancel=1;
            wait(!busy);@(negedge clk);
            if(frame_done || rp_valid || active) $fatal(1,"cancel state=%0d",target);
            cancel=0;
            $display("CANCEL PASS state=%0d",target);
        end
    endtask
    initial begin
        repeat(5)@(negedge clk);rst=0;
        prepare(0);run_case(0,0); // 120B 尾包络，不是 135B。
        prepare(100);run_case(0,100); // 满批次 credit、重复引用与 65534 索引。
        prepare(5);run_case(0,5); // 下一表头位于偏移 14。
        sub_base=32'h100ff0;prepare(0);run_case(0,0);sub_base=32'h100000;
        tri_base=32'h800fa0;prepare(1);run_case(0,1);
        tri_base=32'h800fc0;run_case(0,1);
        tri_base=32'h800fe0;run_case(0,1);
        mode=6;run_case(0,1);mode=0;
        put16(4,65535);run_case(13,0);
        prepare(1);put16(2,1);run_case(13,0);
        prepare(0);sub_bytes=118;run_case(2,0);
        sub_bytes=122;run_case(9,0); // 逻辑尾部多出未解析数据。
        prepare(1);mode=1;run_case(9,0);
        mode=2;run_case(9,0);mode=3;run_case(9,0);
        mode=4;run_case(9,0);mode=5;run_case(9,0);mode=7;run_case(9,0);mode=0;
        prepare(2);
        @(negedge clk);go=1;@(negedge clk);go=0;
        wait(rp_we);@(negedge clk);cancel=1;
        wait(!busy);@(negedge clk);cancel=0;
        if(active || frame_done || rp_valid) $fatal(1,"cancel drain");
        prepare(0);run_case(0,0);
        allow_fd=0;
        @(negedge clk);go=1;@(negedge clk);go=0;
        repeat(4)@(negedge clk);cancel=1;
        wait(!busy);@(negedge clk);cancel=0;allow_fd=1;
        prepare(0);run_case(0,0);
        // 覆盖所有可达事务阶段，含最终 HALT 当拍 cancel 的优先级。
        cancel_state(1);cancel_state(2);cancel_state(3);cancel_state(5);
        cancel_state(7);cancel_state(8);cancel_state(9);cancel_state(10);
        cancel_state(11);cancel_state(12);cancel_state(13);cancel_state(14);
        cancel_state(15);cancel_state(17);cancel_state(16);
        wait(rp_free==3);@(negedge clk);
        tri_count=0;sub_bytes=0;
        begin:zero_frame
            integer old_requests;
            old_requests=requests;
            go=1;@(negedge clk);go=0;
            wait(frame_done);@(negedge clk);
            if(error || requests!=old_requests) $fatal(1,"zero frame issued FD");
        end
        tri_count=65535;
        // 一笔 64 项读若只余 16 项空间，发请求必须被挡住。
        force d.state=1;force d.fifo_wp=48;force d.fifo_rp=0;
        force d.env_total=128;force d.env_left=128;sub_bytes=128;
        #1;if(fd_valid) $fatal(1,"credit reservation overflow");
        release d.state;release d.fifo_wp;release d.fifo_rp;
        release d.env_total;release d.env_left;
        $display("RESULT: PASS rlst descriptors=%0d records=%0d cycles=%0d",requests,delivered,cycle);
        $finish;
    end
endmodule
`default_nettype wire
