// 光栅核定向协议：吞吐、双槽 RAW、长反压、提交沿取消与记录原子拒绝。
`timescale 1ns/1ps
`default_nettype none
module tb_rast_core;
    reg clk=0,rst=1,cancel=0,rp_we=0,rp_valid=0,ready=1,z_clear=0;
    always #5 clk=~clk;
    reg [1:0] rp_slot=0,rp_slot_v=0;
    reg [4:0] rp_wa=0;
    reg [127:0] rp_wd=0;
    wire [1:0] rp_free;
    wire valid,clear_done,err;
    wire [13:0] pix;
    wire [31:0] data,retires,tag;
    wire [3:0] code;
    wire [15:0] index;
    reg [31:0] record[0:31],color[0:10239];
`ifdef GFX_HAS_RAST_TEX
`ifdef RAST_TEX_LAT2
    localparam LAT=2;
`else
    localparam LAT=1;
`endif
    wire samp_valid,samp_ready,samp_rvalid;
    wire [11:0] samp_word;
    wire [31:0] samp_rdata;
    engine_tex #(.READ_LATENCY(LAT)) tex(.clk(clk),.rst(rst),
        .paddr(14'd0),.psel(1'b0),.penable(1'b0),.pwrite(1'b0),.pwdata(32'd0),
        .req_addr(14'd0),.req_bytes(12'd0),.req_buf(2'd0),.req_cidx(4'd0),
        .req_valid(1'b0),.stat_ready(1'b1),.samp_valid(samp_valid),.samp_ready(samp_ready),
        .samp_word(samp_word),.samp_rvalid(samp_rvalid),.samp_rdata(samp_rdata));
`else
    localparam LAT=1;
`endif
    integer commits=0,streak=0,max_streak=0,cycle=0,errors=0,k;
    engine_rast #(.TEX_LATENCY(LAT)) d(.clk(clk),.rst(rst),.cancel(cancel),.rp_we(rp_we),.rp_slot(rp_slot),
        .rp_wa(rp_wa),.rp_wd(rp_wd),.rp_valid(rp_valid),.rp_slot_v(rp_slot_v),
        .rp_index(16'd65534),.rp_strip(5'd0),.rp_ordinal(16'd37),.rp_free(rp_free),
        .ras_wr_valid(valid),.ras_wr_pix(pix),.ras_wr_data(data),.ras_wr_ready(ready),
        .z_clear(z_clear),.z_clear_done(clear_done),.err_valid(err),.err_code(code),
        .err_index(index),.err_tag(tag),.dbg_retire_cnt(retires)
`ifdef GFX_HAS_RAST_TEX
        ,.samp_valid(samp_valid),.samp_ready(samp_ready),.samp_word(samp_word),
        .samp_rvalid(samp_rvalid),.samp_rdata(samp_rdata),.tex_width(13'd64),
        .tex_height(13'd64),.tex_slog2(4'd6),.ras_key(32'hff000000)
`endif
    );
    always @(posedge clk) begin
        cycle<=cycle+1;
        if(valid && ready) begin
            if(cancel || pix>=10240 || data[31:24]!=0) $fatal(1,"commit contract");
            color[pix]<=data;commits<=commits+1;streak<=streak+1;
            if(streak+1>max_streak)max_streak<=streak+1;
        end else streak<=0;
        if(d.zwr_en !== (valid && ready)) $fatal(1,"atomic writes");
        if(err) begin
            if(code!=2 || index!=65534 || tag!={11'd0,5'd0,16'd37}) $fatal(1,"error payload");
            errors<=errors+1;
        end
        if(cycle>100000) $fatal(1,"core watchdog");
    end
    function [15:0] depth;
        input integer p;
        begin case(p&3)
            0:depth=d.g_zbank[0].mem_big[p>>2];1:depth=d.g_zbank[1].mem_big[p>>2];
            2:depth=d.g_zbank[2].mem_big[p>>2];3:depth=d.g_zbank[3].mem_big[p>>2];
        endcase end
    endfunction
    task base_record;
        input integer z;
        begin
            for(integer i=0;i<32;i=i+1)record[i]=0;
`ifdef GFX_HAS_RAST_TEX
            record[0]=3;
`else
            record[0]=2;
`endif
            record[3]=16;record[6]=16;record[9]=16;
            record[10]=z;record[18]={16'd63,16'd0};record[20]=32'hff804020;
        end
    endtask
    task clear;
        begin
            @(negedge clk);z_clear=1;@(negedge clk);z_clear=0;
            wait(clear_done);@(negedge clk);
            for(integer i=0;i<10240;i=i+1)color[i]=32'h12345678;
        end
    endtask
    task deliver;
        input integer slot;
        begin
            wait(rp_free[slot]);@(negedge clk);rp_slot=slot;rp_slot_v=slot;
            for(integer i=0;i<8;i=i+1)begin
                rp_we=1;rp_wa=i*4;
                rp_wd={record[i*4+3],record[i*4+2],record[i*4+1],record[i*4]};
                @(negedge clk);
            end
            rp_we=0;rp_valid=1;@(negedge clk);rp_valid=0;
        end
    endtask
    task reject;
        integer before_errors,before_commits;
        begin
            before_errors=errors;before_commits=commits;
            deliver(0);wait(errors>before_errors);repeat(10)@(negedge clk);
            if(commits!=before_commits || rp_free!=3) $fatal(1,"record partial execution");
            cancel=1;repeat(5)@(negedge clk);cancel=0;repeat(5)@(negedge clk);
        end
    endtask
    initial begin
`ifdef GFX_HAS_RAST_TEX
        for(k=0;k<1024;k=k+1)begin
            tex.g_bank[0].mem[k]=32'h00ffffff;tex.g_bank[1].mem[k]=32'h00ffffff;
            tex.g_bank[2].mem[k]=32'h00ffffff;tex.g_bank[3].mem[k]=32'h00ffffff;
        end
`endif
        repeat(5)@(negedge clk);rst=0;
        clear;base_record(100);deliver(0);wait(retires==1);@(negedge clk);
        if(commits!=64 || max_streak!=64) $fatal(1,"raster II=1 commits=%0d streak=%0d",commits,max_streak);
        for(k=0;k<64;k=k+1) if(color[k]!==32'h00804020) $fatal(1,"constant color");
        clear;ready=0;base_record(100);deliver(0);base_record(200);deliver(1);
        repeat(256)@(negedge clk);
        if(rp_free!=0) $fatal(1,"triangle released with pixels in flight");
        ready=1;wait(retires==3);@(negedge clk);
        for(k=0;k<64;k=k+1) if(depth(k)!==100) $fatal(1,"cross-triangle RAW");
        clear;ready=0;base_record(10);deliver(0);wait(valid);@(negedge clk);
        cancel=1;ready=1;repeat(30)@(negedge clk);cancel=0;
        repeat(10)@(negedge clk);
        for(k=0;k<64;k=k+1) if(color[k]!==32'h12345678 || depth(k)!==65535)
            $fatal(1,"cancel on commit edge");
        base_record(100);record[0]=8;reject;
`ifndef GFX_HAS_RAST_TEX
        base_record(100);record[0]=1;reject;
`endif
        base_record(100);record[11]=32'h40000001;reject;
        base_record(100);record[11]=32'hbfffffff;reject;
        base_record(100);record[31]=1;reject;
        base_record(100);record[30]=32'h00003800;reject;
        base_record(100);record[18]=32'h02800000;reject;
        $display("RESULT: PASS core LAT=%0d II=1 burst=64 RAW=2slots cancel=commit errors=%0d",LAT,errors);
        $finish;
    end
endmodule
`default_nettype wire
