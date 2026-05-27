"""Common style + palette for all twonode plot scripts.

Each plot_*.py should do, near the top:

    import _plot_common as common
    common.apply()
    from _plot_common import STACK_ORDER, STACK_LABEL, STACK_COLOR, clean
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FIGS = os.path.join(ROOT, "paper", "figures")
sys.path.insert(0, FIGS)
from _style import (setup, clean, PALETTE,  # noqa: E402
                    legend_above, legend_above_fig)

apply = setup

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST + TP Bypass",
    "ub_urma":      "UB §8.4 URMA WR",
    "roce_bf":      "RoCE RC (Blue Flame)",
    "roce_dma":     "RoCE RC (DMA WQE fetch)",
}
STACK_COLOR = PALETTE
