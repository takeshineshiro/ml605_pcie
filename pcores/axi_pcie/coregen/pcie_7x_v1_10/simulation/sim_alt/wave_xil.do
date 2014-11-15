if { [info exists PathSeparator] } { set ps $PathSeparator } else { set ps "/" }
set pcie "/board/EP/app/PIO"

set binopt {-logic}
set hexopt {-literal -hex}
set ascopt {-literal -ascii}
set intopt {-literal -signed}

eval add wave -noupdate -divider {"PCIE stream RX"}
eval add wave -noupdate $binopt $pcie${ps}user_clk
eval add wave -noupdate $binopt $pcie${ps}user_reset

eval add wave -noupdate $binopt $pcie${ps}m_axis_rx_tready
eval add wave -noupdate $binopt $pcie${ps}m_axis_rx_tvalid
eval add wave -noupdate $binopt $pcie${ps}m_axis_rx_tlast
eval add wave -noupdate $binopt $pcie${ps}m_axis_rx_tuser
eval add wave -noupdate $binopt \"$pcie${ps}m_axis_rx_tuser(14 downto 10)\"
eval add wave -noupdate $binopt \"$pcie${ps}m_axis_rx_tuser(21 downto 17)\"
eval add wave -noupdate $hexopt \"$pcie${ps}m_axis_rx_tdata(127 downto 96)\"
eval add wave -noupdate $hexopt \"$pcie${ps}m_axis_rx_tdata(95 downto 64)\"
eval add wave -noupdate $hexopt \"$pcie${ps}m_axis_rx_tdata(63 downto 32)\"
eval add wave -noupdate $hexopt \"$pcie${ps}m_axis_rx_tdata(31 downto 0)\"
eval add wave -noupdate $hexopt $pcie${ps}m_axis_rx_tkeep

eval add wave -noupdate -divider {"PCIE stream TX"}
eval add wave -noupdate $binopt $pcie${ps}user_clk
eval add wave -noupdate $binopt $pcie${ps}user_reset

eval add wave -noupdate $binopt $pcie${ps}s_axis_tx_tready
eval add wave -noupdate $binopt $pcie${ps}s_axis_tx_tvalid
eval add wave -noupdate $binopt $pcie${ps}s_axis_tx_tlast
eval add wave -noupdate $binopt $pcie${ps}tx_src_dsc
eval add wave -noupdate $hexopt \"$pcie${ps}s_axis_tx_tdata(127 downto 96)\"
eval add wave -noupdate $hexopt \"$pcie${ps}s_axis_tx_tdata(95 downto 64)\"
eval add wave -noupdate $hexopt \"$pcie${ps}s_axis_tx_tdata(63 downto 32)\"
eval add wave -noupdate $hexopt \"$pcie${ps}s_axis_tx_tdata(31 downto 0)\"
eval add wave -noupdate $hexopt $pcie${ps}s_axis_tx_tkeep

eval add wave -noupdate $hexopt $pcie${ps}cfg_completer_id

eval add wave -noupdate $hexopt $pcie${ps}fc_cpld
eval add wave -noupdate $hexopt $pcie${ps}fc_cplh
eval add wave -noupdate $hexopt $pcie${ps}fc_npd
eval add wave -noupdate $hexopt $pcie${ps}fc_nph
eval add wave -noupdate $hexopt $pcie${ps}fc_pd
eval add wave -noupdate $hexopt $pcie${ps}fc_ph
eval add wave -noupdate $hexopt $pcie${ps}fc_sel
eval add wave -noupdate $hexopt $pcie${ps}tx_buf_av

eval add wave -noupdate -divider {"Slave"}
eval add wave -noupdate $binopt $pcie${ps}user_clk
eval add wave -noupdate $binopt $pcie${ps}user_reset
set prefix s_
set path ${pcie}
do wave_ibus.do

eval add wave -noupdate -divider {"Master"}
eval add wave -noupdate $binopt $pcie${ps}user_clk
eval add wave -noupdate $binopt $pcie${ps}user_reset
set prefix m_
set path ${pcie}
do wave_ibus.do
