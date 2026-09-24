using System;
using System.Reflection;
using System.Reflection.Emit;

namespace HKOracle.Record
{
	// Field readers compiled to IL, for the state recorder's per-frame sweep: FieldInfo.GetValue costs a reflection
	// call and a box per read.  A path is a chain of fields from a root object (or from a static field): reference
	// links are null-checked (a null link reads as 0 / null), value-type links are walked by address, so a struct
	// leaf is read without copying its container.  Nothing is written.
	internal static class FastAccess
	{
		public static long FloatBits(float f) => StateWriter.FloatBits(f);
		public static long DoubleBits(double d) => BitConverter.DoubleToInt64Bits(d);

		private static readonly MethodInfo _floatBits = typeof(FastAccess).GetMethod(nameof(FloatBits));
		private static readonly MethodInfo _doubleBits = typeof(FastAccess).GetMethod(nameof(DoubleBits));

		// Whether Bits() can read a leaf of this type: the scalar types the recorder writes as f/d/i/l/b.
		public static bool IsBitsLeaf(Type t)
		{
			if (t.IsEnum) return true;
			return t == typeof(bool) || t == typeof(byte) || t == typeof(sbyte) || t == typeof(short) || t == typeof(ushort)
				|| t == typeof(char) || t == typeof(int) || t == typeof(uint) || t == typeof(long) || t == typeof(ulong)
				|| t == typeof(float) || t == typeof(double);
		}

		// The leaf as the recorder's raw value: f32/f64 bit patterns; bool, byte, char, ushort zero-extended; sbyte,
		// short, int, uint sign-extended from 32 bits (StateWriter.I stores an int); long and ulong as is.  An enum
		// reads as its underlying type.
		public static Func<object, long> Bits(FieldInfo[] path)
		{
			var leaf = path[path.Length - 1].FieldType;
			var u = leaf.IsEnum ? Enum.GetUnderlyingType(leaf) : leaf;
			var dm = new DynamicMethod("bits_" + path[path.Length - 1].Name, typeof(long), new[] { typeof(object) }, typeof(FastAccess).Module, true);
			var il = dm.GetILGenerator();
			var isNull = il.DefineLabel();
			bool nullable = Walk(il, path, isNull);
			if (u == typeof(float)) il.Emit(OpCodes.Call, _floatBits);
			else if (u == typeof(double)) il.Emit(OpCodes.Call, _doubleBits);
			else if (u == typeof(long) || u == typeof(ulong)) { }
			else if (u == typeof(bool) || u == typeof(byte) || u == typeof(char) || u == typeof(ushort)) il.Emit(OpCodes.Conv_U8);
			else if (u == typeof(uint)) { il.Emit(OpCodes.Conv_I4); il.Emit(OpCodes.Conv_I8); }
			else il.Emit(OpCodes.Conv_I8);
			il.Emit(OpCodes.Ret);
			if (nullable)
			{
				il.MarkLabel(isNull);
				il.Emit(OpCodes.Pop);
				il.Emit(OpCodes.Ldc_I8, 0L);
				il.Emit(OpCodes.Ret);
			}
			return (Func<object, long>)dm.CreateDelegate(typeof(Func<object, long>));
		}

		// The leaf as an object (a value-type leaf is boxed).
		public static Func<object, object> Ref(FieldInfo[] path)
		{
			var leaf = path[path.Length - 1].FieldType;
			var dm = new DynamicMethod("ref_" + path[path.Length - 1].Name, typeof(object), new[] { typeof(object) }, typeof(FastAccess).Module, true);
			var il = dm.GetILGenerator();
			var isNull = il.DefineLabel();
			bool nullable = Walk(il, path, isNull);
			if (leaf.IsValueType) il.Emit(OpCodes.Box, leaf);
			il.Emit(OpCodes.Ret);
			if (nullable)
			{
				il.MarkLabel(isNull);
				il.Emit(OpCodes.Pop);
				il.Emit(OpCodes.Ldnull);
				il.Emit(OpCodes.Ret);
			}
			return (Func<object, object>)dm.CreateDelegate(typeof(Func<object, object>));
		}

		// An IntPtr instance field as its value (UnityEngine.Object.m_CachedPtr).
		public static Func<object, long> IntPtrField(FieldInfo f)
		{
			var dm = new DynamicMethod("ptr_" + f.Name, typeof(long), new[] { typeof(object) }, typeof(FastAccess).Module, true);
			var il = dm.GetILGenerator();
			il.Emit(OpCodes.Ldarg_0);
			il.Emit(OpCodes.Castclass, f.DeclaringType);
			il.Emit(OpCodes.Ldfld, f);
			il.Emit(OpCodes.Conv_I8);
			il.Emit(OpCodes.Ret);
			return (Func<object, long>)dm.CreateDelegate(typeof(Func<object, long>));
		}

		// Loads the leaf; a null reference link branches to isNull with that null on the stack.
		private static bool Walk(ILGenerator il, FieldInfo[] path, Label isNull)
		{
			bool nullable = false;
			for (int i = 0; i < path.Length; i++)
			{
				var f = path[i];
				bool last = i == path.Length - 1;
				if (f.IsStatic)
				{
					if (i != 0) throw new ArgumentException("a static field must start the path");
					il.Emit(!last && f.FieldType.IsValueType ? OpCodes.Ldsflda : OpCodes.Ldsfld, f);
				}
				else
				{
					if (i == 0)
					{
						il.Emit(OpCodes.Ldarg_0);
						if (f.DeclaringType.IsValueType) throw new ArgumentException("a path starts at a reference type");
						il.Emit(OpCodes.Castclass, f.DeclaringType);
					}
					il.Emit(!last && f.FieldType.IsValueType ? OpCodes.Ldflda : OpCodes.Ldfld, f);
				}
				if (!last && !f.FieldType.IsValueType)
				{
					il.Emit(OpCodes.Dup);
					il.Emit(OpCodes.Brfalse, isNull);
					nullable = true;
				}
			}
			return nullable;
		}
	}
}
