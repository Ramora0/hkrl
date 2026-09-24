using System.Reflection;

namespace HKOracle
{
	// Which build of the mod is running. HKOracle.csproj stamps the git commit of the tree it was built
	// from into AssemblyInformationalVersion ("1.0.0.0+<sha>", "+<sha>-dirty" when oracle/ had uncommitted
	// changes); traces, draw logs and dumps record it so a recording names the mod that made it.
	internal static class ModInfo
	{
		private static string _commit;

		internal static string Commit
		{
			get
			{
				if (_commit != null) return _commit;
				string v = null;
				try
				{
					var a = typeof(ModInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>();
					v = a?.InformationalVersion;
				}
				catch { v = null; }
				int plus = v == null ? -1 : v.IndexOf('+');
				_commit = plus < 0 ? "" : v.Substring(plus + 1);
				return _commit;
			}
		}
	}
}
