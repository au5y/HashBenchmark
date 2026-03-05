import json
import matplotlib.pyplot as plt
import sys
from collections import defaultdict

def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_benchmarks.py <results.json>")
        return
    
    with open(sys.argv[1], 'r') as f:
        data = json.load(f)
        
    results = defaultdict(lambda: {'x': [], 'y': []})
    
    for bench in data.get('benchmarks', []):
        name = bench['name']
        if '/' not in name: 
            continue
            
        algo, size_str = name.split('/')
        size = int(size_str)
        time = bench['cpu_time']
        
        results[algo]['x'].append(size)
        results[algo]['y'].append(time)
        
    plt.figure(figsize=(10, 6))
    
    for algo, vals in results.items():
        sorted_pairs = sorted(zip(vals['x'], vals['y']))
        x = [p[0] for p in sorted_pairs]
        y = [p[1] for p in sorted_pairs]
        
        plt.plot(x, y, marker='o', label=algo, linewidth=2, markersize=6)
        
    plt.xscale('log') 
    plt.xlabel('Number of IDs Configured (N)')
    plt.ylabel('CPU Time per 10k Lookups (ns)')
    plt.title('Compile-Time Message Dispatch Performance by Table Size')
    plt.legend()
    plt.grid(True, which="both", ls="--", alpha=0.6)
    
    output_file = 'benchmark_plot.png'
    plt.savefig(output_file, dpi=300)

if __name__ == "__main__":
    main()