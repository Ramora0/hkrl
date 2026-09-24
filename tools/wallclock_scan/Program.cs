using System;
using System.IO;
using System.Linq;
using HKOracle.Record;
using Mono.Cecil;

static class Program
{
	static int Main(string[] args)
	{
		if (args.Length != 2) { Console.Error.WriteLine("usage: WallClockScan <Managed dir> <out.jsonl>"); return 2; }
		var resolver = new DefaultAssemblyResolver();
		resolver.AddSearchDirectory(args[0]);
		var sites = WallClockSites.Scan(WallClockSites.GamePaths(args[0]), resolver);
		File.WriteAllLines(args[1], sites.Select(WallClockSites.ToJson));
		Console.WriteLine($"wallclock scan: {sites.Count} sites in {sites.Select(s => s.Full).Distinct().Count()} methods -> {args[1]}");
		return 0;
	}
}
