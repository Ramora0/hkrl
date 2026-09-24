using System;
using System.Globalization;
using System.Reflection;

namespace HKOracle.Record
{
	// A pure reader of UnityEngine.Random's four xorshift128 state words.  Random.state's getter copies them;
	// the fields of Random.State are private, so a DynamicMethod reads them without boxing.
	public static class RngState
	{
		private delegate void GetStateFastDelegate(out int s0, out int s1, out int s2, out int s3);
		private static GetStateFastDelegate _fast;
		private static FieldInfo[] _fields;
		private static bool _resolved;

		public static string Describe => _fields != null && _fields.Length == 4
			? $"{_fields[0].Name}, {_fields[1].Name}, {_fields[2].Name}, {_fields[3].Name}" : "unbound";

		// Throws when Random.State does not have the expected shape: callers log it at install.
		public static void Resolve()
		{
			if (_resolved) return;
			_resolved = true;
			var st = typeof(UnityEngine.Random).GetNestedType("State", BindingFlags.Public | BindingFlags.NonPublic);
			var getMethod = typeof(UnityEngine.Random).GetProperty("state", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)?.GetGetMethod(true);
			if (st == null || getMethod == null) throw new InvalidOperationException("RNG state type/getter not found");
			_fields = st.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
			if (_fields == null || _fields.Length != 4) throw new InvalidOperationException("RNG state has != 4 fields");
			var dm = new System.Reflection.Emit.DynamicMethod("GetRngStateFast", null,
				new[] { typeof(int).MakeByRefType(), typeof(int).MakeByRefType(), typeof(int).MakeByRefType(), typeof(int).MakeByRefType() },
				typeof(RngState).Module, true);
			var il = dm.GetILGenerator();
			var loc = il.DeclareLocal(st);
			il.Emit(System.Reflection.Emit.OpCodes.Call, getMethod);
			il.Emit(System.Reflection.Emit.OpCodes.Stloc, loc);
			for (int i = 0; i < 4; i++)
			{
				il.Emit(System.Reflection.Emit.OpCodes.Ldarg, i);
				il.Emit(System.Reflection.Emit.OpCodes.Ldloca_S, loc);
				il.Emit(System.Reflection.Emit.OpCodes.Ldfld, _fields[i]);
				il.Emit(System.Reflection.Emit.OpCodes.Stind_I4);
			}
			il.Emit(System.Reflection.Emit.OpCodes.Ret);
			_fast = (GetStateFastDelegate)dm.CreateDelegate(typeof(GetStateFastDelegate));
		}

		public static void Read(out uint s0, out uint s1, out uint s2, out uint s3)
		{
			int a, b, c, d;
			if (_fast != null) _fast(out a, out b, out c, out d);
			else
			{
				object state = UnityEngine.Random.state;
				a = Convert.ToInt32(_fields[0].GetValue(state), CultureInfo.InvariantCulture);
				b = Convert.ToInt32(_fields[1].GetValue(state), CultureInfo.InvariantCulture);
				c = Convert.ToInt32(_fields[2].GetValue(state), CultureInfo.InvariantCulture);
				d = Convert.ToInt32(_fields[3].GetValue(state), CultureInfo.InvariantCulture);
			}
			s0 = (uint)a; s1 = (uint)b; s2 = (uint)c; s3 = (uint)d;
		}
	}
}
