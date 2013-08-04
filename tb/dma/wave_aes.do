set binopt {-logic}
set hexopt {-literal -hex}
if { [info exists PathSeparator] } { set ps $PathSeparator } else { set ps "/" }
if { ![info exists aespath] } { set aespath "/dma_tb_tb${ps}dut/axi_aes_0/axi_aes_0" }

 eval add wave -noupdate -divider {"axi_aes_0 lite "}
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_aclk
 eval add wave -noupdate $binopt $aespath${ps}m_axi_mm2s_aclk
 eval add wave -noupdate $binopt $aespath${ps}m_axi_s2mm_aclk
 eval add wave -noupdate $binopt $aespath${ps}axi_resetn
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_awvalid
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_awready
 eval add wave -noupdate $hexopt $aespath${ps}s_axi_lite_awaddr
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_wvalid
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_wready
 eval add wave -noupdate $hexopt $aespath${ps}s_axi_lite_wdata
 eval add wave -noupdate $hexopt $aespath${ps}s_axi_lite_bresp
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_bvalid
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_bready
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_arvalid
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_arready
 eval add wave -noupdate $hexopt $aespath${ps}s_axi_lite_araddr
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_rvalid
 eval add wave -noupdate $binopt $aespath${ps}s_axi_lite_rready
 eval add wave -noupdate $hexopt $aespath${ps}s_axi_lite_rdata
 eval add wave -noupdate $hexopt $aespath${ps}s_axi_lite_rresp
 
 eval add wave -noupdate -divider {"axi_aes_0 mm2s "}
 eval add wave -noupdate $binopt $aespath${ps}mm2s_prmry_reset_out_n
 eval add wave -noupdate $hexopt $aespath${ps}m_axis_mm2s_tdata
 eval add wave -noupdate $hexopt $aespath${ps}m_axis_mm2s_tkeep
 eval add wave -noupdate $binopt $aespath${ps}m_axis_mm2s_tvalid
 eval add wave -noupdate $binopt $aespath${ps}m_axis_mm2s_tready
 eval add wave -noupdate $binopt $aespath${ps}m_axis_mm2s_tlast
 eval add wave -noupdate $hexopt $aespath${ps}m_axis_mm2s_tuser
 eval add wave -noupdate $hexopt $aespath${ps}m_axis_mm2s_tid
 eval add wave -noupdate $hexopt $aespath${ps}m_axis_mm2s_tdest
 
 eval add wave -noupdate -divider {"axi_aes_0 mm2s cntrl "}
 eval add wave -noupdate $binopt $aespath${ps}mm2s_cntrl_reset_out_n
 eval add wave -noupdate $hexopt $aespath${ps}m_axis_mm2s_cntrl_tdata
 eval add wave -noupdate $hexopt $aespath${ps}m_axis_mm2s_cntrl_tkeep
 eval add wave -noupdate $binopt $aespath${ps}m_axis_mm2s_cntrl_tvalid
 eval add wave -noupdate $binopt $aespath${ps}m_axis_mm2s_cntrl_tready
 eval add wave -noupdate $binopt $aespath${ps}m_axis_mm2s_cntrl_tlast

 eval add wave -noupdate -divider {"axi_aes_0 s2mm"}
 eval add wave -noupdate $binopt $aespath${ps}s2mm_prmry_reset_out_n
 eval add wave -noupdate $hexopt $aespath${ps}s_axis_s2mm_tdata
 eval add wave -noupdate $hexopt $aespath${ps}s_axis_s2mm_tkeep
 eval add wave -noupdate $binopt $aespath${ps}s_axis_s2mm_tvalid
 eval add wave -noupdate $binopt $aespath${ps}s_axis_s2mm_tready
 eval add wave -noupdate $binopt $aespath${ps}s_axis_s2mm_tlast
 eval add wave -noupdate $hexopt $aespath${ps}s_axis_s2mm_tuser
 eval add wave -noupdate $hexopt $aespath${ps}s_axis_s2mm_tid
 eval add wave -noupdate $hexopt $aespath${ps}s_axis_s2mm_tdest
 
 eval add wave -noupdate -divider {"axi_aes_0 s2mm sts"}
 eval add wave -noupdate $binopt $aespath${ps}s2mm_sts_reset_out_n
 eval add wave -noupdate $hexopt $aespath${ps}s_axis_s2mm_sts_tdata
 eval add wave -noupdate $hexopt $aespath${ps}s_axis_s2mm_sts_tkeep
 eval add wave -noupdate $binopt $aespath${ps}s_axis_s2mm_sts_tvalid
 eval add wave -noupdate $binopt $aespath${ps}s_axis_s2mm_sts_tready
 eval add wave -noupdate $binopt $aespath${ps}s_axis_s2mm_sts_tlast

