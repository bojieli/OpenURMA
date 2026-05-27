"""Shared style for OpenURMA paper figures.

All figure scripts should `from _style import *` (or import setup()).
Defines a consistent palette (UB cool / RoCE warm), readable fonts at
two-column 3.3-inch width, and a thin minimal-clutter grid.
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


# --- Palette ---------------------------------------------------------------
# UB stacks: cool colours (blue family, green for URMA);
# RoCE stacks: warm colours (orange Blue-Flame, red DMA).
PALETTE = {
    "ub_loadstore": "#1f77b4",  # deep blue
    "ub_urma":      "#2ca02c",  # green
    "roce_bf":      "#ff7f0e",  # orange
    "roce_dma":     "#d62728",  # red
}

LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST",
    "ub_urma":      "UB §8.4 URMA WR",
    "roce_bf":      "RoCE RC (BF)",
    "roce_dma":     "RoCE RC (DMA)",
}

# Categorical colours for non-stack uses (e.g. policy/mode comparisons).
CAT = ["#1f77b4", "#2ca02c", "#d62728", "#ff7f0e",
       "#9467bd", "#8c564b", "#17becf", "#e377c2"]


def setup():
    """Apply rcParams. Call once at the top of every figure script."""
    plt.rcParams.update({
        "font.family":       "serif",
        "font.serif":        ["Times", "DejaVu Serif"],
        "mathtext.fontset":  "stix",
        "font.size":         8.5,
        "axes.titlesize":    9,
        "axes.labelsize":    8.5,
        "xtick.labelsize":   7.5,
        "ytick.labelsize":   7.5,
        "legend.fontsize":   7,
        "legend.frameon":    True,
        "legend.framealpha": 0.92,
        "legend.fancybox":   False,
        "legend.edgecolor":  "#bbbbbb",
        "axes.linewidth":    0.7,
        "axes.grid":         True,
        "grid.linewidth":    0.35,
        "grid.alpha":        0.5,
        "grid.color":        "#cccccc",
        "lines.linewidth":   1.4,
        "lines.markersize":  4,
        "pdf.fonttype":      42,
        "ps.fonttype":       42,
        "savefig.bbox":      "tight",
        "savefig.pad_inches": 0.03,
    })


def clean(ax):
    """Despine top/right; thin spines; minor grid off."""
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_linewidth(0.7)
        ax.spines[side].set_color("#444444")
    ax.tick_params(width=0.6, length=3, color="#444444")


def legend_above(ax, handles=None, labels=None, ncol=None, fontsize=None,
                 pad=1.02, title_pad=None, budget=58, **kw):
    """Place a legend above the axes, outside the data area.

    The legend is frameless and centred over the column, so it can never
    overlap plotted data. ``ncol`` is chosen from a rough per-row
    character ``budget`` (total characters across all columns of one row)
    unless given explicitly; widen ``budget`` for full-width subplots. If
    the axes already carries a title, pass ``title_pad`` (axes-fraction y
    for ``set_title``) to lift it clear of the legend; otherwise the caller
    should drop the title. With ``savefig.bbox='tight'`` the extra height
    is captured, not clipped.
    """
    if handles is None or labels is None:
        h, l = ax.get_legend_handles_labels()
        handles = h if handles is None else handles
        labels = l if labels is None else labels
    if not labels:
        return None
    n = len(labels)
    if ncol is None:
        longest = max(len(str(x)) for x in labels)
        ncol = max(1, min(n, budget // (longest + 3)))
    opts = dict(loc="lower center", bbox_to_anchor=(0.5, pad), ncol=ncol,
                frameon=False, borderaxespad=0.0, columnspacing=1.1,
                handlelength=1.5, handletextpad=0.5, labelspacing=0.3)
    if fontsize is not None:
        opts["fontsize"] = fontsize
    opts.update(kw)
    leg = ax.legend(handles, labels, **opts)
    if title_pad is not None and ax.get_title():
        ax.set_title(ax.get_title(), y=title_pad)
    return leg


def legend_above_fig(fig, handles, labels, ncol=None, fontsize=None,
                     y=1.0, budget=110, **kw):
    """Single shared legend centred above an entire (multi-panel) figure.

    Use for figures whose panels share one set of series; per-panel
    titles stay below it. ``budget`` is wider than the per-axes default
    because the legend spans the full figure width. Call after
    ``tight_layout`` so the panels are already positioned.
    """
    if not labels:
        return None
    n = len(labels)
    if ncol is None:
        longest = max(len(str(x)) for x in labels)
        ncol = max(1, min(n, budget // (longest + 3)))
    opts = dict(loc="lower center", bbox_to_anchor=(0.5, y), ncol=ncol,
                frameon=False, borderaxespad=0.0, columnspacing=1.4,
                handlelength=1.6, handletextpad=0.5)
    if fontsize is not None:
        opts["fontsize"] = fontsize
    opts.update(kw)
    return fig.legend(handles, labels, **opts)


# Standard figure sizes for two-column layout.
SINGLE = (3.35, 2.3)        # one column, compact
SINGLE_TALL = (3.35, 2.6)   # one column, slightly taller
DOUBLE = (7.0, 2.6)         # full width, low aspect
DOUBLE_TALL = (7.0, 3.0)    # full width, normal aspect
