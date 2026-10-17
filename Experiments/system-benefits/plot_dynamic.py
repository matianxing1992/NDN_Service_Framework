#!/usr/bin/env python3
"""One grayscale figure showing completion, conditional delay and total traffic."""
import argparse
import json
from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import LogLocator, FuncFormatter

ARMS=("SOLICIT","ADS100","ADS1000","ADS_EVENT")
LABELS=("Solicitation","Ads: 100 ms","Ads: 1 s","Ads: change + 1 s")


def plot_completion(summary, output):
    """Show all configurations at fast switching, retaining both workloads."""
    data = json.loads(summary.read_text())
    if not data["qualified"]:
        raise ValueError("unqualified matrix")
    rows = {(r["period_ms"], r["dwell_ms"], r["arm"]): r for r in data["results"]}
    plt.rcParams.update({"font.family": "serif", "font.serif": ["Times New Roman", "DejaVu Serif"],
        "font.size": 8, "axes.labelsize": 8, "xtick.labelsize": 8, "ytick.labelsize": 8,
        "pdf.fonttype": 42})
    figure, axis = plt.subplots(figsize=(3.4, 2.45))
    colors = ("white", "0.75", "black", "white")
    markers = ("o", "s", "^", "D")
    for index, arm in enumerate(ARMS):
        for group, period in enumerate((0, 4700)):
            estimate = rows[(period, 500, arm)]["metrics"]["success_fraction"]
            x = group + (index - 1.5) * .14
            if estimate["mean"] is None:
                axis.text(x, 3, "NA", ha="center", fontsize=7)
                continue
            half = estimate["ci95_half_width"]
            axis.errorbar(x, estimate["mean"] * 100,
                yerr=half * 100 if half is not None else None,
                fmt=markers[index], markersize=4.8, markerfacecolor=colors[index],
                markeredgecolor="black", markeredgewidth=.7, color="black",
                linestyle="none", elinewidth=.7, capsize=2, capthick=.7, zorder=3)
            if estimate["n"] < 5:
                axis.annotate(f'n={estimate["n"]}', (x, estimate["mean"] * 100),
                    xytext=(0, 4), textcoords="offset points", ha="center", fontsize=6)
    axis.set_xticks((0, 1))
    axis.set_xticklabels(("Continuous", "Paced: 4.7 s"))
    axis.set_ylabel("Completed tasks (%)")
    axis.set_ylim(0, 105)
    axis.set_xlim(-.4, 1.4)
    axis.set_yticks((0, 25, 50, 75, 100))
    axis.grid(axis="y", color="0.87", linewidth=.5, zorder=0)
    axis.spines[["top", "right"]].set_visible(False)
    legend = [Line2D([], [], color="black", marker=markers[i], linestyle="none",
        markersize=4.8, markerfacecolor=colors[i], markeredgewidth=.7, label=LABELS[i])
        for i in range(4)]
    figure.legend(handles=legend, loc="upper center", ncol=2, frameon=False,
                  columnspacing=1, handletextpad=.3, bbox_to_anchor=(.5, 1.01))
    figure.tight_layout(rect=(0, 0, 1, .83))
    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, bbox_inches="tight")
    plt.close(figure)


def plot(summary,output):
    data=json.loads(summary.read_text())
    if not data["qualified"]:raise ValueError("unqualified matrix")
    rows={(r["period_ms"],r["dwell_ms"],r["arm"]):r for r in data["results"]}
    plt.rcParams.update({"font.family":"serif","font.serif":["Times New Roman","DejaVu Serif"],
        "font.size":8,"axes.labelsize":8,"xtick.labelsize":7,"ytick.labelsize":7,"pdf.fonttype":42})
    figure,axes=plt.subplots(2,3,figsize=(7.05,3.9))
    colors=("white","0.75","black","white");markers=("o","s","^","D")
    metrics=(("success_fraction",100,"Completed tasks (%)"),
             ("conditional_latency_p50_ms",1,"Mean block median (ms)"),
             ("window_ip_bytes_per_success",1/1024,"IP KiB / completion (log)"))
    cost_bounds=[]
    for value in rows.values():
        estimate=value["metrics"]["window_ip_bytes_per_success"]
        if estimate["mean"] is None:continue
        half=estimate["ci95_half_width"] or 0
        low,high=(estimate["mean"]-half)/1024,(estimate["mean"]+half)/1024
        if low<=0:raise ValueError("nonpositive cost interval cannot use the logarithmic axis")
        cost_bounds.append((low,high))
    if not cost_bounds:raise ValueError("no defined completion cost")
    cost_limits=(min(v[0] for v in cost_bounds)/1.3,max(v[1] for v in cost_bounds)*1.3)
    delay_high=max((r["metrics"]["conditional_latency_p50_ms"]["mean"] or 0)+
                   (r["metrics"]["conditional_latency_p50_ms"]["ci95_half_width"] or 0)
                   for r in rows.values())*1.07
    for row,period in enumerate((0,4700)):
        for column,(metric,scale,title) in enumerate(metrics):
            axis=axes[row,column]
            for index,arm in enumerate(ARMS):
                for group,dwell in enumerate((0,5000,500)):
                    estimate=rows[(period,dwell,arm)]["metrics"][metric]
                    x=group+(index-1.5)*.18
                    if estimate["mean"] is None:
                        axis.text(x,.03,"NA (n=0)",rotation=90,ha="center",va="bottom",transform=axis.get_xaxis_transform(),fontsize=6)
                        continue
                    half=estimate["ci95_half_width"]
                    axis.errorbar(x,estimate["mean"]*scale,yerr=half*scale if half is not None else None,
                        fmt=markers[index],markersize=4.8,markerfacecolor=colors[index],
                        markeredgecolor="black",markeredgewidth=.7,color="black",linestyle="none",
                        elinewidth=.6,capsize=1.5,capthick=.6,zorder=3)
                    if estimate["n"] < 5:
                        axis.annotate(f'n={estimate["n"]}',(x,estimate["mean"]*scale),
                            xytext=(0,3),textcoords="offset points",ha="center",fontsize=6)
            axis.set_xticks(range(3));axis.set_xticklabels(("Stable","5 s dwell","0.5 s dwell"))
            axis.set_title(title,pad=4)
            axis.grid(axis="y",color="0.87",linewidth=.5,zorder=0)
            axis.spines[["top","right"]].set_visible(False)
            if column==0:axis.set_ylim(0,105)
            elif column==1:axis.set_ylim(0,delay_high)
            else:
                axis.set_yscale("log");axis.set_ylim(*cost_limits)
                axis.yaxis.set_major_locator(LogLocator(base=10,subs=(1,3)))
                axis.yaxis.set_major_formatter(FuncFormatter(lambda value,_:f"{value:g}"))
            if column==0:axis.set_ylabel("Closed loop" if period==0 else "Paced: 4.7 s")
    legend=[Line2D([],[],color="black",marker=markers[i],linestyle="none",markersize=4.8,
                   markerfacecolor=colors[i],markeredgewidth=.7,label=LABELS[i]) for i in range(4)]
    figure.legend(handles=legend,loc="upper center",ncol=4,frameon=False,bbox_to_anchor=(.5,1.005))
    figure.tight_layout(rect=(0,0,1,.93),h_pad=1.5,w_pad=1)
    output.parent.mkdir(parents=True,exist_ok=True);figure.savefig(output,bbox_inches="tight");plt.close(figure)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("summary",type=Path);parser.add_argument("output",type=Path)
    parser.add_argument("--completion-only", action="store_true",
                        help="Compact fast-switching completion figure; retain all four arms")
    args=parser.parse_args()
    (plot_completion if args.completion_only else plot)(args.summary,args.output)
