// 条带数据面独立端点：完整 RAM、状态保持、地址/配置错误、同步端口冲突与取消。
// DMA 端为事务级激励；真实 AXI/排空在完整 mover 测试中另验。
`timescale 1ns/1ps
`default_nettype none
module tb_engine_band;
    reg clk=0,rst=1,cancel=0,err_clr=0;always #5 clk=~clk;
    reg start=0,flush=0;
    wire start_ready,flush_ready,loaded,flushed,active,busy,loading,flushing,error;
    wire [3:0] error_code;
    reg [31:0] bg_base=32'h1000,dst_base=32'h20000;
    reg [4:0] bg_sl=12,dst_sl=12;
    reg [15:0] y0=16,frame_h=480;
    wire [31:0] dr_addr,dw_addr;wire [11:0] dr_bytes,dw_bytes;
    wire dr_valid,dw_valid;
    wire dr_ready=1'b1;
    wire dw_ready=1'b1;
    reg [127:0] d_rdata=0;reg d_rvalid=0,d_rlast=0,d_rdone=0,d_rerr=0,d_wdone=0,d_werr=0;
    wire [127:0] d_wdata;wire d_wvalid,d_rready;
    reg hold_w=0;reg [7:0] tick=0;
    wire d_wready=!hold_w && tick[2:0]!=0;
    reg r_pending=0,w_pending=0;
    wire d_idle=!r_pending && !w_pending;
    reg [31:0] rp,wp;
    integer rleft,wleft,rcount=0,wcount=0;
    reg [127:0] expected[0:2559];
    reg [31:0] ra=0,wa=0;reg [11:0] rn=16,wn=16;
    reg [1:0] rbuf=2'b10;reg [3:0] rcidx=7,wcidx=9;
    reg rv=0,wv=0,wlast=1;wire rready,wready,wsready,rbusy,wbusy;
    reg [127:0] wd=0;reg [15:0] wk=16'hffff;reg wvalid=0;
    wire [127:0] rq;wire rvalid,rlast,rstat,rerr,wstat,werr,wstatlast;
    wire [1:0] rqbuf,rstatbuf;wire [3:0] rstattag,wstattag;
    reg rstatready=0,wstatready=0;
    integer errors=0,i,p,cy,rd_seen=0,check_index=0,read_expected=0,blocked=0;
    reg check_reads=0;
    engine_band dut(.clk(clk),.rst(rst),.cancel(cancel),.err_clr(err_clr),.start(start),.start_ready(start_ready),
        .flush(flush),.flush_ready(flush_ready),.bg_base(bg_base),.dst_base(dst_base),.bg_slog2(bg_sl),.dst_slog2(dst_sl),
        .strip_y0(y0),.frame_height(frame_h),.loaded(loaded),.flushed(flushed),.active(active),.busy(busy),
        .loading(loading),.flushing(flushing),.error(error),.error_code(error_code),
        .dma_rd_addr(dr_addr),.dma_rd_bytes(dr_bytes),.dma_rd_valid(dr_valid),.dma_rd_ready(dr_ready),
        .dma_rdata(d_rdata),.dma_rvalid(d_rvalid),.dma_rlast(d_rlast),.dma_rready(d_rready),
        .dma_rd_done(d_rdone),.dma_rd_error(d_rerr),.dma_wr_addr(dw_addr),.dma_wr_bytes(dw_bytes),
        .dma_wr_valid(dw_valid),.dma_wr_ready(dw_ready),.dma_wdata(d_wdata),.dma_wvalid(d_wvalid),.dma_wready(d_wready),
        .dma_wr_done(d_wdone),.dma_wr_error(d_werr),.dma_idle(d_idle),
        .rd_addr(ra),.rd_bytes(rn),.rd_buf(rbuf),.rd_cidx(rcidx),.rd_valid(rv),.rd_ready(rready),
        .rs_data(rq),.rs_valid(rvalid),.rs_last(rlast),.rs_buf(rqbuf),.rd_stat_valid(rstat),.rd_stat_error(rerr),
        .rd_stat_cidx(rstattag),.rd_stat_buf(rstatbuf),.rd_stat_ready(rstatready),.rd_busy(rbusy),
        .wr_addr(wa),.wr_bytes(wn),.wr_cidx(wcidx),.wr_last_row(wlast),.wr_valid(wv),.wr_ready(wready),
        .ws_data(wd),.ws_keep(wk),.ws_valid(wvalid),.ws_ready(wsready),
        .wr_stat_valid(wstat),.wr_stat_error(werr),.wr_stat_cidx(wstattag),.wr_stat_last_row(wstatlast),
        .wr_stat_ready(wstatready),.wr_busy(wbusy));
    function [127:0] pattern;
        input [31:0] a;
        integer k;
        begin
            for(k=0;k<4;k=k+1) pattern[k*32 +:32]=32'hc0000000 | ((a+k*32'h12345)&32'h00ffffff);
        end
    endfunction
    always @(posedge clk) begin
        tick<=tick+1'b1;d_rvalid<=0;d_rdone<=0;d_wdone<=0;
        if(!rst) begin
            if(dr_valid && dr_ready) begin
                r_pending<=1;rp<=dr_addr;rleft<=dr_bytes>>4;rcount=rcount+1;
            end
            if(r_pending) begin
                d_rdata<=pattern(rp);d_rvalid<=1;d_rlast<=rleft==1;rp<=rp+16;rleft<=rleft-1;
                if(rleft==1) begin r_pending<=0;d_rdone<=1;end
            end
            if(dw_valid && dw_ready) begin
                w_pending<=1;wp<=dw_addr;wleft<=dw_bytes>>4;wcount=wcount+1;
            end
            if(d_wvalid && d_wready) begin
                i=((wp-dst_base)>>12)-y0;
                i=i*160+((wp-dst_base)&4095)/16;
                if(i<0 || i>=2560 || d_wdata!==expected[i]) begin errors=errors+1;$display("UNIT-FLUSH-DATA index=%0d",i);end
                wp<=wp+16;wleft<=wleft-1;
                if(wleft==1) begin w_pending<=0;d_wdone<=1;end
            end
            if(check_reads && rvalid) begin
                if(rq!==expected[check_index+rd_seen] || rqbuf!=rbuf || rlast!=(rd_seen==read_expected-1)) begin
                    errors=errors+1;$display("UNIT-READ-DATA index=%0d",check_index+rd_seen);
                end
                rd_seen=rd_seen+1;
            end
            if(wvalid && !wsready && dut.ram_re && dut.ram_ra==dut.write_index) blocked=blocked+1;
        end
    end
    task begin_load;
        begin
            @(negedge clk);start=1;@(posedge clk);while(!start_ready) @(posedge clk);@(negedge clk);start=0;
            cy=0;while(!loaded && !error && cy<10000) begin @(negedge clk);cy=cy+1;end
            if(!loaded) begin errors=errors+1;$display("UNIT-LOAD");end
        end
    endtask
    task read_words;
        input integer idx,n;
        begin
            @(negedge clk);check_index=idx;read_expected=n;rd_seen=0;check_reads=1;rstatready=0;
            ra=dst_base+(y0+idx/160)*4096+(idx%160)*16;rn=n*16;rv=1;
            @(posedge clk);while(!rready) @(posedge clk);@(negedge clk);rv=0;
            cy=0;while(!rstat && cy<1000) begin @(negedge clk);cy=cy+1;end
            repeat(3) begin
                @(negedge clk);
                if(!rstat || rerr || rstattag!=rcidx || rstatbuf!=rbuf || rready || rd_seen!=n) begin
                    errors=errors+1;$display("UNIT-READ-STATUS");
                end
            end
            rstatready=1;@(negedge clk);rstatready=0;check_reads=0;
        end
    endtask
    task write_word;
        input integer idx;
        input [127:0] data;
        input [15:0] keep;
        input expected_err;
        begin
            @(negedge clk);wa=dst_base+(y0+idx/160)*4096+(idx%160)*16;wn=16;wv=1;wstatready=0;
            @(posedge clk);while(!wready) @(posedge clk);@(negedge clk);wv=0;wd=data;wk=keep;wvalid=1;
            @(posedge clk);while(!wsready) @(posedge clk);@(negedge clk);wvalid=0;
            cy=0;while(!wstat && cy<1000) begin @(negedge clk);cy=cy+1;end
            repeat(3) begin
                @(negedge clk);
                if(!wstat || werr!=expected_err || wstattag!=wcidx || !wstatlast || wready) begin errors=errors+1;$display("UNIT-WRITE-STATUS");end
            end
            wstatready=1;@(negedge clk);wstatready=0;
        end
    endtask
    initial begin
        for(i=0;i<2560;i=i+1) expected[i]=pattern(bg_base+(y0+i/160)*4096+(i%160)*16);
        repeat(5) @(negedge clk);rst=0;
        // 配置错误不得发 DMA，随后能合法重试。
        y0=470;start=1;@(negedge clk);start=0;@(negedge clk);
        if(!error || error_code!=1 || busy || rcount!=0) begin errors=errors+1;$display("UNIT-CFG");end
        y0=16;begin_load();
        read_words(0,160);read_words(1920,160);read_words(2559,1);
        // 同址并发时读优先，写等同步读取结束；不依赖器件碰撞模式。
        @(negedge clk);check_index=2048;read_expected=1;rd_seen=0;check_reads=1;
        ra=dst_base+(y0+2048/160)*4096+(2048%160)*16;rn=16;rv=1;
        wa=ra;wn=16;wv=1;rstatready=0;wstatready=0;
        @(posedge clk);if(!rready || !wready) begin errors=errors+1;$display("UNIT-CONFLICT-DESC");end
        @(negedge clk);rv=0;wv=0;wd={4{32'h0013579b}};wk=16'hffff;wvalid=1;
        @(posedge clk);while(!wsready) @(posedge clk);@(negedge clk);wvalid=0;
        while(!rstat || !wstat) @(negedge clk);
        if(blocked!=1 || rd_seen!=1 || rerr || werr) begin errors=errors+1;$display("UNIT-CONFLICT blocked=%0d reads=%0d",blocked,rd_seen);end
        check_reads=0;expected[2048]={4{32'h0013579b}};
        rstatready=1;wstatready=1;@(negedge clk);rstatready=0;wstatready=0;
        read_words(2048,1);
        write_word(0,{32'h80000000,32'h00224466,32'h80909090,32'h00112233},16'hffff,0);
        expected[0][31:0]=32'h00112233;expected[0][95:64]=32'h00224466;
        read_words(0,1);
        // 无效地址拒绝并保持原 RAM。
        @(negedge clk);ra=dst_base+y0*4096+2560;rn=16;rv=1;
        @(posedge clk);while(!rready) @(posedge clk);@(negedge clk);rv=0;
        while(!rstat) @(negedge clk);
        if(!rerr || rstattag!=rcidx || rvalid) begin errors=errors+1;$display("UNIT-RANGE");end
        rstatready=1;@(negedge clk);rstatready=0;err_clr=1;@(negedge clk);err_clr=0;
        write_word(1,128'h1234,16'hfff0,1);
        err_clr=1;@(negedge clk);err_clr=0;read_words(1,1);
        // 原始写回必须保留未改像素的 X 高字节，且反压保持输出。
        @(negedge clk);flush=1;@(posedge clk);while(!flush_ready) @(posedge clk);@(negedge clk);flush=0;
        cy=0;while(!flushed && cy<10000) begin @(negedge clk);cy=cy+1;end
        if(!flushed || busy || wcount!=16) begin errors=errors+1;$display("UNIT-FLUSH");end
        // LOAD 已接受后取消，等事务 idle 再重新装载。
        @(negedge clk);start=1;@(posedge clk);while(!start_ready) @(posedge clk);@(negedge clk);start=0;
        wait(r_pending);@(negedge clk);cancel=1;
        repeat(3) @(negedge clk);
        if(!busy || loaded || rready) begin errors=errors+1;$display("UNIT-CANCEL-WAIT");end
        cy=0;while(busy && cy<1000) begin @(negedge clk);cy=cy+1;end
        if(busy) begin errors=errors+1;$display("UNIT-CANCEL");end
        cancel=0;bg_sl=13;begin_load();
        // 背景 8192 stride、目的 4096 stride：配置独立，不能共用游标步长。
        for(i=0;i<2560;i=i+1) expected[i]=pattern(bg_base+(y0+i/160)*8192+(i%160)*16);
        read_words(0,1);read_words(1920,160);read_words(2559,1);
        @(negedge clk);flush=1;@(posedge clk);while(!flush_ready) @(posedge clk);@(negedge clk);flush=0;
        cy=0;while(!flushed && cy<10000) begin @(negedge clk);cy=cy+1;end
        if(!flushed || busy || wcount!=32) begin errors=errors+1;$display("UNIT-DUAL-STRIDE");end
        $display("BAND UNIT errors=%0d reads=%0d writes=%0d",errors,rcount,wcount);
        if(errors==0) $display("RESULT: PASS");else $display("RESULT: FAIL");
        $finish;
    end
    initial begin #20000000;$display("WATCHDOG");$finish;end
endmodule
`default_nettype wire
