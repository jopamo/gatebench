#!/usr/bin/env python3
"""
gatebench comparison tool
Reads JSON results from multiple gatebench runs and generates comparison tables/plots.
"""

import json
import sys
import os
import argparse
from pathlib import Path
from typing import Dict, List, Any, Optional
import csv

try:
    import matplotlib.pyplot as plt
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("Warning: matplotlib not installed, plotting disabled")

def load_results(filepath: str) -> Dict[str, Any]:
    """Load JSON results from a file."""
    with open(filepath, 'r') as f:
        return json.load(f)

def extract_key_data(results: Dict[str, Any]) -> Dict[str, Any]:
    """Extract key metrics from results."""
    data = {
        'entries': results.get('config', {}).get('entries', 0),
        'iters': results.get('config', {}).get('iters', 0),
        'runs': results.get('config', {}).get('runs', 0),
        'interval_ns': results.get('config', {}).get('interval_ns', 0),
    }
    
    # Extract summary statistics
    summary = results.get('summary', {})
    data.update({
        'median_ops_per_sec': summary.get('median_ops_per_sec', 0),
        'min_ops_per_sec': summary.get('min_ops_per_sec', 0),
        'max_ops_per_sec': summary.get('max_ops_per_sec', 0),
        'stddev_ops_per_sec': summary.get('stddev_ops_per_sec', 0),
        'median_p50_ns': summary.get('median_p50_ns', 0),
        'median_p95_ns': summary.get('median_p95_ns', 0),
        'median_p99_ns': summary.get('median_p99_ns', 0),
        'median_p999_ns': summary.get('median_p999_ns', 0),
    })
    
    # Extract environment info
    env = results.get('environment', {})
    data.update({
        'kernel': f"{env.get('sysname', '')} {env.get('release', '')}",
        'machine': env.get('machine', ''),
    })
    
    return data

def print_comparison_table(results_list: List[Dict[str, Any]]) -> None:
    """Print comparison table to console."""
    print("\n" + "="*120)
    print("GATEBENCH COMPARISON RESULTS")
    print("="*120)
    
    headers = [
        "Entries", "Ops/sec (med)", "Ops/sec (min)", "Ops/sec (max)", 
        "p50 (ns)", "p95 (ns)", "p99 (ns)", "Kernel"
    ]
    
    # Print header
    header_fmt = "{:>8} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12} {:>20}"
    print(header_fmt.format(*headers))
    print("-"*120)
    
    # Print data rows
    row_fmt = "{:>8} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f} {:>12.0f} {:>20}"
    for data in results_list:
        kernel_short = data['kernel'].split()[1] if ' ' in data['kernel'] else data['kernel']
        if len(kernel_short) > 20:
            kernel_short = kernel_short[:17] + "..."
        
        print(row_fmt.format(
            data['entries'],
            data['median_ops_per_sec'],
            data['min_ops_per_sec'],
            data['max_ops_per_sec'],
            data['median_p50_ns'],
            data['median_p95_ns'],
            data['median_p99_ns'],
            kernel_short
        ))
    
    print("="*120)

def generate_csv(output_file: str, results_list: List[Dict[str, Any]]) -> None:
    """Generate CSV file with comparison data."""
    fieldnames = [
        'entries', 'iters', 'runs', 'interval_ns',
        'median_ops_per_sec', 'min_ops_per_sec', 'max_ops_per_sec', 'stddev_ops_per_sec',
        'median_p50_ns', 'median_p95_ns', 'median_p99_ns', 'median_p999_ns',
        'kernel', 'machine'
    ]
    
    with open(output_file, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for data in results_list:
            writer.writerow(data)
    
    print(f"\nCSV report saved to: {output_file}")

def plot_results(results_list: List[Dict[str, Any]], output_dir: str) -> None:
    """Generate plots from results."""
    if not HAS_MATPLOTLIB:
        print("Skipping plots: matplotlib not installed")
        return
    
    # Sort by entries
    results_list.sort(key=lambda x: x['entries'])
    
    entries = [r['entries'] for r in results_list]
    ops_median = [r['median_ops_per_sec'] for r in results_list]
    ops_min = [r['min_ops_per_sec'] for r in results_list]
    ops_max = [r['max_ops_per_sec'] for r in results_list]
    
    p50 = [r['median_p50_ns'] / 1000 for r in results_list]  # Convert to microseconds
    p95 = [r['median_p95_ns'] / 1000 for r in results_list]
    p99 = [r['median_p99_ns'] / 1000 for r in results_list]
    
    # Create figure with 2 subplots
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 10))
    
    # Plot 1: Operations per second
    ax1.plot(entries, ops_median, 'b-o', label='Median ops/sec', linewidth=2)
    ax1.fill_between(entries, ops_min, ops_max, alpha=0.2, label='Min-Max range')
    ax1.set_xscale('log', base=2)
    ax1.set_xlabel('Number of gate entries')
    ax1.set_ylabel('Operations per second')
    ax1.set_title('Gate Control-Plane Performance vs. Schedule Size')
    ax1.grid(True, alpha=0.3)
    ax1.legend()
    
    # Plot 2: Latency percentiles
    ax2.plot(entries, p50, 'g-o', label='p50 (median)', linewidth=2)
    ax2.plot(entries, p95, 'r-s', label='p95', linewidth=2)
    ax2.plot(entries, p99, 'm-^', label='p99', linewidth=2)
    ax2.set_xscale('log', base=2)
    ax2.set_xlabel('Number of gate entries')
    ax2.set_ylabel('Latency (μs)')
    ax2.set_title('Latency Percentiles vs. Schedule Size')
    ax2.grid(True, alpha=0.3)
    ax2.legend()
    
    plt.tight_layout()
    
    # Save plot
    plot_file = os.path.join(output_dir, 'gatebench_comparison.png')
    plt.savefig(plot_file, dpi=150)
    print(f"Plot saved to: {plot_file}")
    
    # Show plot if in interactive mode
    if 'DISPLAY' in os.environ:
        plt.show()

def main():
    parser = argparse.ArgumentParser(
        description='Compare gatebench results from multiple JSON files'
    )
    parser.add_argument(
        'files',
        nargs='+',
        help='JSON result files to compare'
    )
    parser.add_argument(
        '--csv',
        help='Output CSV file for comparison data'
    )
    parser.add_argument(
        '--plot',
        action='store_true',
        help='Generate comparison plots (requires matplotlib)'
    )
    parser.add_argument(
        '--output-dir',
        default='.',
        help='Directory for output files (default: current directory)'
    )
    
    args = parser.parse_args()
    
    # Load and process all result files
    all_results = []
    for filepath in args.files:
        try:
            results = load_results(filepath)
            data = extract_key_data(results)
            data['source_file'] = os.path.basename(filepath)
            all_results.append(data)
            print(f"Loaded: {filepath} (entries={data['entries']})")
        except (json.JSONDecodeError, FileNotFoundError) as e:
            print(f"Error loading {filepath}: {e}", file=sys.stderr)
    
    if not all_results:
        print("No valid results to compare", file=sys.stderr)
        sys.exit(1)
    
    # Print comparison table
    print_comparison_table(all_results)
    
    # Generate CSV if requested
    if args.csv:
        generate_csv(args.csv, all_results)
    else:
        # Default CSV name
        csv_file = os.path.join(args.output_dir, 'gatebench_comparison.csv')
        generate_csv(csv_file, all_results)
    
    # Generate plots if requested
    if args.plot:
        plot_results(all_results, args.output_dir)
    
    # Print summary
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    
    # Find best and worst performing configurations
    if len(all_results) > 1:
        best_ops = max(all_results, key=lambda x: x['median_ops_per_sec'])
        worst_ops = min(all_results, key=lambda x: x['median_ops_per_sec'])
        
        best_latency = min(all_results, key=lambda x: x['median_p50_ns'])
        worst_latency = max(all_results, key=lambda x: x['median_p50_ns'])
        
        print(f"Best throughput: {best_ops['entries']} entries "
              f"({best_ops['median_ops_per_sec']:.0f} ops/sec)")
        print(f"Worst throughput: {worst_ops['entries']} entries "
              f"({worst_ops['median_ops_per_sec']:.0f} ops/sec)")
        print(f"Best latency: {best_latency['entries']} entries "
              f"({best_latency['median_p50_ns']:.0f} ns p50)")
        print(f"Worst latency: {worst_latency['entries']} entries "
              f"({worst_latency['median_p50_ns']:.0f} ns p50)")
    
    print("="*60)

if __name__ == '__main__':
    main()