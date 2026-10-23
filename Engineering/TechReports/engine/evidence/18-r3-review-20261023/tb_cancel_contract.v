`timescale 1ns/1ps
`default_nettype none
// Contract experiment, not an engine_top integration test. The source audit
// checks that mover drain masks FS; this responder models that one effect.
module tb_cancel_contract;
    reg clk=0, rst=1, cancel=0, go=0;
    always #5 clk=~clk;
    wire fd_valid, fs_ready, busy;
    wire [11:0] fd_bytes;
    reg fs_valid=0, fs_last=0, st_valid=0;
    engine_rlst dut(.clk(clk),.rst(rst),.cancel(cancel),.go(go),
        .tri_base(32'h10000),.sub_base(32'h20000),.sub_bytes(24'd120),.tri_count(16'd1),
        .fd_bytes(fd_bytes),.fd_valid(fd_valid),.fd_ready(1'b1),
        .fs_data(128'd0),.fs_valid(fs_valid),.fs_last(fs_last),.fs_keep(16'hffff),
        .fs_cidx(4'd0),.fs_ready(fs_ready),.st_valid(st_valid),.st_error(1'b0),
        .st_cidx(4'd0),.rp_free(2'b11),.strip_ack(1'b0),.busy(busy));
    integer beats, i;
    task begin_frame;
        begin
            @(negedge clk); rst=1; cancel=0; go=0; fs_valid=0; st_valid=0;
            repeat(3) @(negedge clk);
            rst=0;go=1;
            @(negedge clk); go=0;
            wait(fd_valid); beats=fd_bytes/16;
            @(posedge clk); @(negedge clk); cancel=1;
        end
    endtask
    initial begin
        // Global mover drain swallowed every FS beat but delivered its status.
        begin_frame;
        st_valid=1;
        @(negedge clk);st_valid=0;
        repeat(128) @(negedge clk);
        if(!busy) $fatal(1,"lost FS unexpectedly drained");
        $display("WITNESS lost FS: mover complete but actual rlst busy=1");
        // Only reset this local state after physical mover completion is known.
        rst=1;
        repeat(2) @(negedge clk);
        rst=0;
        if(busy) $fatal(1,"post-mover reset did not recover");
        // With its ordinary stream contract retained, cancel drains cleanly.
        begin_frame;
        for(i=0;i<beats;i=i+1) begin
            wait(fs_ready);
            fs_valid=1;fs_last=(i==beats-1);
            @(posedge clk);@(negedge clk);fs_valid=0;fs_last=0;
            // Allow the actual one-u16-per-cycle serialization to finish.
            repeat(10) @(negedge clk);
        end
        st_valid=1;
        @(negedge clk);st_valid=0;
        repeat(128) @(negedge clk);
        if(busy) $fatal(1,"forwarded cancel did not drain");
        $display("RESULT: PASS cancellation contract witness and recovery");
        $finish;
    end
    initial begin #100000; $fatal(1,"watchdog");end
endmodule
`default_nettype wire
