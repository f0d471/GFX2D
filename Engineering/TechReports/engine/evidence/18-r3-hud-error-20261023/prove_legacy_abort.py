from pathlib import Path
import sys,shutil,subprocess
root=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(root/'Main/tools/check'))
import feature_build as fb,feature_config as fc,regs_gen
from r3_vectors import generate
out=root/'Main/sim/build/r3-dev'/('legacy-abort-fixed' if '--fixed' in sys.argv else 'legacy-abort-proof');out.mkdir(parents=True,exist_ok=True)
tree=out/'rtl';shutil.copytree(root/'Main/rtl',tree,dirs_exist_ok=True)
cfg=fc.parse_name('B7');fb.write_feature_files(cfg,fc.name_of(cfg),tree/'engine')
(tree/'engine/engine_regs_offsets.vh').write_text(regs_gen.gen_vh(fc.caps(cfg)))
generate(out/'vectors',1,True)
s=(root/'Main/sim/engine/r3/tb_r3.v').read_text().split('        if($test$plusargs("PROTOCOLS"))begin')[0]
s+='''
        setup(0);put(LIST,20);
        for(k=0;k<20;k=k+1)begin
            put(LIST+32768+k*32,32'h00100001);
            put(LIST+32768+k*32+4,0);put(LIST+32768+k*32+8,32'h00080000);
            put(LIST+32768+k*32+12,32'h00010008);put(LIST+32768+k*32+16,32'h00fedcba);
            put(LIST+32768+k*32+20,0);put(LIST+32768+k*32+24,0);
            put(LIST+32768+k*32+28,32'hcafe0000+k);
        end
        put(LIST+32768,32'h00100077);put(LIST+32768+28,32'habcdef01);
        write_reg(16'h028,1);
        wait(dut.u_cmd.fe_owner==1 && dut.old_fd_valid);
        @(negedge clk);hold_ar=1;
        wait(dut.u_cmd.st==12 && dut.u_cmd.pend_err);
        @(negedge clk);
        if(!dut.u_cmd.fe_busy_any || !dut.u_mover.rd_busy)$fatal(1,"fixture missing pending prefetch");
        repeat(128)begin
            if(dut.r3_drain || dut.band_abort)$fatal(1,"EARLY LEGACY ABORT steals pending command prefetch");
            @(negedge clk);
        end
        hold_ar=0;finish(1,0);error_tuple(0,32'habcdef01);
        $display("RESULT: PASS legacy prefetch preserved before raster abort");$finish;
    end
    initial begin #100000000; $fatal(1,"watchdog");end
endmodule
`default_nettype wire
'''
(out/'tb.v').write_text(s)
exclude={Path(x).name for f,files in fc.FEATURE_SOURCES.items() if not cfg[f] for x in files}
sources=[x for x in (tree/'engine').glob('*.v') if x.name not in exclude]
sources += [tree/'vendor/verilog-axi'/x for x in ('axi_dma_rd.v','axi_dma_wr.v')]
sources += [root/'Main/sim/axi_probe/axi_ram_model.v']
with (out/'compile.log').open('w') as log:
    subprocess.run(['iverilog','-g2012','-Wportbind','-I',str(tree/'engine'),'-s','tb_r3','-o',str(out/'tb.vvp'),str(out/'tb.v'),*map(str,sources)],stdout=log,stderr=subprocess.STDOUT,check=True)
r=subprocess.run(['vvp','-n',str(out/'tb.vvp'),'+FRAMES=0',f'+VECTORS={out/"vectors"}'],capture_output=True,text=True,timeout=120)
(out/'result.log').write_text(r.stdout+r.stderr)
print(r.stdout+r.stderr)
if '--fixed' in sys.argv:
    assert r.returncode==0 and 'RESULT: PASS legacy' in r.stdout
else:
    assert r.returncode and 'EARLY LEGACY ABORT' in r.stdout
    print('EXPECTED RED: real pending CMD prefetch would be discarded by immediate R3 drain')
