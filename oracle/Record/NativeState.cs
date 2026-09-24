using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using Newtonsoft.Json.Linq;
using UnityEngine;

namespace HKOracle.Record
{
	// Read-only view of the engine's native state (UnityPlayer.dll 2020.2.2f1), through UnityEngine.Object.m_CachedPtr
	// and the PDB layouts of analysis/decomp_native/types/*.layout.txt.  Nothing here writes engine memory or calls
	// an engine function.  Every read goes through Mem, which checks that the page is committed and readable, so a
	// wrong layout ends the native section with a NOTE instead of crashing the game.
	//
	// Addresses are RVAs from the preferred base 0x180000000 (analysis/decomp_native/symbols.tsv); the running base
	// is GetModuleHandle("UnityPlayer.dll").  Check() validates the layouts against the managed API before any of
	// this is trusted (docs/state-record.md "Layout check").
	internal sealed class NativeState
	{
		// ---------------------------------------------------------------- RVAs (symbols.tsv)
		private const long RVA_gContext = 0x19d7a30;          // D gContext; GetManagerFromContext(i) = gContext[i] (UP!0x180572d00)
		private const int CTX_TimeManager = 7;               // GetTimeManager: mov ecx,7; jmp GetManagerFromContext (UP!0x18052b330)
		private const int CTX_DelayedCallManager = 8;        // GetDelayedCallManager: mov ecx,8 (UP!0x180629ef0)
		private static readonly long[] RVA_BehaviourManagers = {
			0x1a12a40,   // s_instanceBehaviourManager (Update)       Behaviour::InitializeClass UP!0x18062a0a0
			0x1a12a48,   // s_instanceFixedBehaviourManager
			0x1a12a50,   // s_instanceLateBehaviourManager
			0x1a12a58 }; // s_instanceUpdateManager
		private static readonly string[] BehaviourManagerNames = { "Update", "FixedUpdate", "LateUpdate", "UpdateManager" };
		private static readonly long[] VT_BehaviourManagers = { 0x167d638, 0x167d648, 0x167d658, 0x167d638 };   // ??_7BehaviourManager/Fixed/Late
		private const long VT_TimeManager = 0x16727f8, VT_DelayedCallManager = 0x167d668;
		private static readonly Dictionary<long, string> CallNames = new Dictionary<long, string> {
			{ 0x8ac680, "MonoBehaviour::DelayedStartCall" }, { 0x8a6c70, "Coroutine::ContinueCoroutine" },
			{ 0x629d80, "DelayedDestroyCallback" }, { 0x8c0ea0, "ForwardInvokeDelayed" },
			{ 0x8a6950, "Coroutine::CleanupCoroutine" }, { 0x8a6a30, "Coroutine::CleanupCoroutineGC" },
			{ 0x8c0f00, "ForwardInvokeDelayedCleanup" }, { 0x74d4c0, "AsyncOperation::CleanupCoroutine" } };
		private const long RVA_ContinueCoroutine = 0x8a6c70, RVA_ForwardInvokeDelayed = 0x8c0ea0;
		// D gIPhysics2D (GetPhysicsManager2D returns it, UP!0x180c04120); D TransformChangeDispatch::gTransformChangeDispatch;
		// ??_7PhysicsManager2D@@6B@
		private const long RVA_gIPhysics2D = 0x1a17ee8, RVA_gTransformChangeDispatch = 0x1a1ddd8, VT_PhysicsManager2D = 0x16ff1a0;

		// ---------------------------------------------------------------- layouts (types/*.layout.txt)
		// Object / Component / Behaviour / MonoBehaviour (playerloop.layout.txt)
		private const int OBJ_InstanceID = 0x08, COMP_GameObject = 0x30, BEH_Enabled = 0x38, BEH_IsAdded = 0x39;
		private const int MB_ActiveCoroutines = 0x70, MB_UpdateNode = 0x80, MB_FixedNode = 0x98, MB_LateNode = 0xb0,
			MB_DidAwake = 0x124, MB_DidStart = 0x125, MB_IsDestroying = 0x127, MB_AddedToManager = 0x128;
		// GameObject (playerloop.h: EditorExtension 0x30, dynamic_array m_Component 0x20, uint m_Layer, ushort m_Tag,
		// bool m_IsActive); +0x50 m_Layer is also the physics2d spec's E9 asm read [[collider+0x30]+0x50]
		private const int GO_Layer = 0x50, GO_IsActive = 0x56;
		// Transform (transform.layout.txt): the live local TRS is in the TransformHierarchy, not in Transform's own
		// fields (native-transform_time.md section 1): m_TransformData {hierarchy +0, index +8} at +0x38;
		// hierarchy localTransforms +0x18 (trsX stride 0x30: t +0, q +0x10, s +0x20), mainThreadOnlyTransformPointers +0x30
		private const int TR_Access = 0x38, TH_Local = 0x18, TH_Pointers = 0x30, TRS_Size = 0x30;
		// TransformHierarchy change bookkeeping: transformCapacity +0x10, changeDispatchIndex +0x38, per-index
		// ulong64 systemChanged[] +0x48 and systemInterested[] +0x50 (Transform.c:1827-1831 copies them per index)
		private const int TH_Capacity = 0x10, TH_DispatchIndex = 0x38, TH_SystemChanged = 0x48, TH_SystemInterested = 0x50;
		// TransformChangeDispatch (transform.layout.txt): m_CombinedSystemChangedMask +0, m_Hierarchies (dynamic_array)
		// +0x8, m_SystemInUseMask +0x80
		private const int TCD_Combined = 0x0, TCD_Hierarchies = 0x8, TCD_InUse = 0x80;
		// PhysicsManager2D (physics2d.layout.txt): the change-system handles PhysicsManager2D::Initialize registers
		// (physics2d/_free.c: TransformChangeDispatch::RegisterSystem x5, TransformHierarchyChangeDispatch::RegisterSystem x2)
		private const int PM_RbT = 0xc, PM_RbR = 0x10, PM_RbS = 0x14, PM_RbAnim = 0x18, PM_ColTRS = 0x1c, PM_RbParent = 0x20, PM_ColParent = 0x24;
		// Coroutine (playerloop.layout.txt): ListElement at +0, m_CoroutineEnumeratorGCHandle at +0x10:
		// ScriptingGCHandle {u64 m_Handle +0, ScriptingGCHandleWeakness m_Weakness +8, MonoObject* m_Object +0x10}
		private const int CO_EnumGCHandle = 0x10, GCH_Weakness = 0x8, GCH_Object = 0x10, GCHANDLE_STRONG = 2, CO_Behaviour = 0x58, CO_RefCount = 0x60, CO_Done = 0x64,
			CO_ContinueWhenFinished = 0x68, CO_WaitingFor = 0x70, CO_AsyncOp = 0x78, CO_IsIEnumerator = 0x80;
		// TimeManager (time.layout.txt): TimeHolder{cur d +0, last d +8, curUnscaled d +0x10, delta f +0x18, unscaledDelta f +0x1c}
		private const int TM_Fixed = 0x30, TM_Dynamic = 0x60, TM_Active = 0x90, TM_FirstFrameAfterReset = 0xc0,
			TM_FirstFrameAfterPause = 0xc1, TM_FirstFixedFrameAfterReset = 0xc2, TM_FrameCount = 0xc8,
			TM_CaptureDeltaTime = 0xd8, TM_SceneLoadOffset = 0xf0, TM_UseFixedTimeStep = 0xf9, TM_TimeScale = 0xfc,
			TM_MaximumTimestep = 0x100;
		// DelayedCallManager (playerloop.layout.txt): multiset _Myhead +0x30 / _Mysize +0x38, m_TimeStamp +0x48;
		// _Tree_node: _Left +0, _Parent +8, _Right +0x10, _Isnil +0x19, value +0x20 (Callback, 0x40 bytes)
		private const int DCM_Head = 0x30, DCM_Size = 0x38, DCM_TimeStamp = 0x48, TN_Left = 0, TN_Parent = 8,
			TN_Right = 0x10, TN_IsNil = 0x19, TN_Value = 0x20;
		// BaseBehaviourManager: map m_Lists at +0x8 (_Myhead +0x8, _Mysize +0x10); value pair<int, pair<List*,List*>>:
		// key +0x20, active list +0x28, add list +0x30 (native-playerloop.md Check: "the map node holds the active
		// list at +0x28 and the add list at +0x30").  ListNode<Behaviour>: ListElement{prev,next} + m_Data +0x10.
		private const int BM_Head = 0x8, BM_Size = 0x10, BMN_Key = 0x20, BMN_Active = 0x28, BMN_Add = 0x30, LN_Data = 0x10;
		// Rigidbody2D / Collider2D (physics2d.layout.txt)
		private const int RB_GravityScale = 0x44, RB_Simulated = 0x48, RB_BodyType = 0x5c, RB_Body = 0x78, RB_Movement = 0x98,
			RB_ParentDrivenBy = 0x118, RB_Scene = 0x120;
		private const int COL_Offset = 0x44, COL_Density = 0x4c, COL_IsTrigger = 0x50, COL_ErrorState = 0x54, COL_Shapes = 0x58,
			COL_RelativeTransform = 0x78, COL_RigidbodyScale = 0xb8, COL_Scene = 0xd8, BOX_Size = 0x120, BOX_EdgeRadius = 0x128,
			CIRCLE_Radius = 0xe0, EDGE_EdgeRadius = 0xe0;
		// PhysicsScene2D / PhysicsContacts2D / Collision2D (physics2d.layout.txt)
		private const int PS_Handle = 0x4, PS_World = 0x8, PS_Ground = 0x10, PS_Contacts = 0x28, PS_MoveStates = 0x70,
			PS_LastSimTime = 0x490, PS_LastSimDelta = 0x498, PS_Running = 0x49c, PS_HierarchyChanged = 0x49d;
		private const int PC_Collisions = 0x28, PC_SimulationId = 0x50;
		private const int C2D_Size = 0x68, MF_Size = 0x70;
		// b2World (box2d.layout.txt); contact manager at +0x192a8, broadphase first in it, tree first in that
		private const int W_Flags = 0x192a0, W_CM = 0x192a8, W_BodyList = 0x19380, W_BodyCount = 0x19390,
			W_NonStatic = 0x19398, W_Static = 0x193b8, W_Gravity = 0x193d8, W_AllowSleep = 0x193e0, W_InvDt0 = 0x193f8,
			W_WarmStarting = 0x193fc, W_Continuous = 0x193fd, W_SubStepping = 0x193fe, W_StepComplete = 0x193ff,
			W_DiscreteIslands = 0x19400, W_ContinuousIslands = 0x19404;
		private const int CM_ContactList = 0x78, CM_NonTOI = 0x98, CM_TOI = 0xb8;
		private const int BP_ProxyCount = 0x28, BP_PairBuffer = 0x30, BP_MoveBuffer = 0x50, BP_QueryProxyId = 0x70;
		private const int TREE_Root = 0x0, TREE_Nodes = 0x8, TREE_NodeCount = 0x10, TREE_Capacity = 0x14, TREE_FreeList = 0x18,
			TREE_Path = 0x1c, TREE_Insertions = 0x20, NODE_Size = 0x28;
		private const int BODY_Size = 0xc8, FIX_Size = 0x58, PROXY_Size = 0x20, CONTACT_Size = 0x138;
		// dynamic_array: ptr +0, size +0x10
		private const int DA_Ptr = 0x0, DA_Size = 0x10;

		// ---------------------------------------------------------------- state
		private readonly StateWriter _w;
		private readonly Action<string> _log;
		private static readonly FieldInfo _cachedPtr = typeof(UnityEngine.Object).GetField("m_CachedPtr", BindingFlags.Instance | BindingFlags.NonPublic);
		private static readonly Func<object, long> _cachedPtrGet = MakePtrGetter();
		private static Func<object, long> MakePtrGetter()
		{
			if (_cachedPtr == null) return null;
			try { return FastAccess.IntPtrField(_cachedPtr); }
			catch { return o => ((IntPtr)_cachedPtr.GetValue(o)).ToInt64(); }
		}
		private long _base;
		public bool Enabled { get; private set; }
		public string DisabledWhy { get; private set; }
		public readonly JObject Checks = new JObject();
		private long _scene;   // PhysicsScene2D*
		private readonly Dictionary<long, int> _proxyOwner = new Dictionary<long, int>();   // b2FixtureProxy* -> fixture eid
		private readonly Dictionary<long, int> _proxyChild = new Dictionary<long, int>();
		private readonly Dictionary<long, HashSet<long>> _coroutinesOf = new Dictionary<long, HashSet<long>>();   // MonoBehaviour* -> its m_ActiveCoroutines, this frame
		private long _pm, _tcd;              // PhysicsManager2D*, TransformChangeDispatch*
		private readonly int[] _pmHandles = new int[7];
		private ulong _physMask;             // the five TransformChangeDispatch bits PhysicsManager2D::SyncTransforms drains
		public Func<object, int, string, int, int> Plain;   // StateRecorder.PlainEid(obj, parentEid, key, depth)

		// entity key namespaces (StateWriter.Key)
		private const int NS_BODY = 2, NS_FIXTURE = 3, NS_CONTACT = 4, NS_COLLISION = 5, NS_TREENODE = 6, NS_COROUTINE = 7,
			NS_CALL = 8, NS_SINGLE = 9;

		public NativeState(StateWriter w, Action<string> log) { _w = w; _log = log; }

		public JObject Info()
		{
			var rva = new JObject();
			foreach (var kv in CallNames) rva["0x" + kv.Key.ToString("x")] = kv.Value;
			return new JObject { ["module_base"] = "0x" + _base.ToString("x"), ["enabled"] = Enabled, ["disabled_why"] = DisabledWhy,
				["call_rvas"] = rva, ["checks"] = Checks };
		}

		public void Disable(string why)
		{
			if (!Enabled && DisabledWhy != null) return;
			Enabled = false;
			DisabledWhy = why;
			_log("native section OFF: " + why);
			try { _w.Note("native OFF: " + why); } catch { }
		}

		public long Ptr(UnityEngine.Object o)
		{
			if (ReferenceEquals(o, null) || _cachedPtrGet == null) return 0;
			return _cachedPtrGet(o);
		}

		// ================================================================ memory
		[DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
		[DllImport("kernel32.dll")] private static extern IntPtr VirtualQuery(IntPtr addr, out MBI info, IntPtr len);
		[StructLayout(LayoutKind.Sequential)]
		private struct MBI { public IntPtr BaseAddress, AllocationBase; public uint AllocationProtect, Pad1; public IntPtr RegionSize; public uint State, Protect, Type, Pad2; }

		internal sealed class BadRead : Exception { public BadRead(string m) : base(m) { } }

		// readable regions seen this frame, sorted by start (disjoint: VirtualQuery regions)
		private readonly long[] _lo = new long[4096], _hi = new long[4096];
		private int _nreg, _hit;

		private void Chk(long a, int n)
		{
			if (a < 0x10000 || a > 0x7FFFFFFF0000L) throw new BadRead($"pointer 0x{a:x}");
			long end = a + n;
			if (_hit < _nreg && a >= _lo[_hit] && end <= _hi[_hit]) return;
			int lo = 0, hi = _nreg - 1;
			while (lo <= hi)
			{
				int mid = (lo + hi) >> 1;
				if (_lo[mid] > a) hi = mid - 1;
				else if (_hi[mid] <= a) lo = mid + 1;
				else { if (end <= _hi[mid]) { _hit = mid; return; } break; }
			}
			long p = a;
			while (p < end)
			{
				if (VirtualQuery(new IntPtr(p), out MBI m, new IntPtr(Marshal.SizeOf(typeof(MBI)))) == IntPtr.Zero)
					throw new BadRead($"VirtualQuery 0x{p:x}");
				const uint COMMIT = 0x1000, READABLE = 0x02 | 0x04 | 0x08 | 0x20 | 0x40 | 0x80, GUARD = 0x100;
				if (m.State != COMMIT || (m.Protect & READABLE) == 0 || (m.Protect & GUARD) != 0) throw new BadRead($"unreadable 0x{p:x}");
				long rlo = m.BaseAddress.ToInt64(), rhi = rlo + m.RegionSize.ToInt64();
				if (_nreg == _lo.Length) _nreg = 0;   // full: start over (the cache is per frame anyway)
				int at = 0;
				while (at < _nreg && _lo[at] < rlo) at++;
				if (at < _nreg && _lo[at] == rlo) { _hi[at] = rhi; }
				else
				{
					Array.Copy(_lo, at, _lo, at + 1, _nreg - at);
					Array.Copy(_hi, at, _hi, at + 1, _nreg - at);
					_lo[at] = rlo; _hi[at] = rhi;
					_nreg++;
				}
				_hit = at;
				p = rhi;
			}
		}

		private long P(long a) { Chk(a, 8); return Marshal.ReadInt64(new IntPtr(a)); }
		private int I32(long a) { Chk(a, 4); return Marshal.ReadInt32(new IntPtr(a)); }
		private long I64(long a) { Chk(a, 8); return Marshal.ReadInt64(new IntPtr(a)); }
		private byte U8(long a) { Chk(a, 1); return Marshal.ReadByte(new IntPtr(a)); }
		private float F32(long a) => BitsToFloat(I32(a));
		private double F64(long a) => BitConverter.Int64BitsToDouble(I64(a));
		private byte[] Bytes(long a, int n) { Chk(a, n); var b = new byte[n]; Marshal.Copy(new IntPtr(a), b, 0, n); return b; }

		[StructLayout(LayoutKind.Explicit)] private struct FI { [FieldOffset(0)] public float F; [FieldOffset(0)] public int I; }
		private static float BitsToFloat(int i) => new FI { I = i }.F;
		private static int FloatToBits(float f) => new FI { F = f }.I;
		private static long FB(byte[] b, int o) => (long)(uint)BitConverter.ToInt32(b, o);   // f32 bits as a list element

		private int Iid(long obj) => obj == 0 ? 0 : I32(obj + OBJ_InstanceID);

		public void BeginFrame() { _nreg = 0; _hit = 0; _coroutinesOf.Clear(); }

		// ================================================================ install and layout check
		public void Init()
		{
			try
			{
				_base = GetModuleHandle("UnityPlayer.dll").ToInt64();
				if (_base == 0) { Disable("UnityPlayer.dll not loaded"); return; }
				if (_cachedPtr == null) { Disable("UnityEngine.Object.m_CachedPtr not found"); return; }
				Enabled = true;
				_pm = P(_base + RVA_gIPhysics2D);
				_tcd = P(_base + RVA_gTransformChangeDispatch);
				int[] offs = { PM_RbT, PM_RbR, PM_RbS, PM_RbAnim, PM_ColTRS, PM_RbParent, PM_ColParent };
				for (int i = 0; i < offs.Length; i++) _pmHandles[i] = I32(_pm + offs[i]);
				_physMask = 0;
				for (int i = 0; i < 5; i++) _physMask |= 1UL << (_pmHandles[i] & 0x3f);   // PhysicsManager2D::ClearTransformChanges UP!0x180bdcb20
			}
			catch (Exception e) { Disable("init: " + e.Message); }
		}

		private sealed class Tally { public int Ok, Bad; public JArray Examples = new JArray(); }
		private readonly Dictionary<string, Tally> _tally = new Dictionary<string, Tally>();

		private void Expect(string name, bool ok, string detail)
		{
			if (!_tally.TryGetValue(name, out Tally t)) _tally[name] = t = new Tally();
			if (ok) t.Ok++;
			else { t.Bad++; if (t.Examples.Count < 4) t.Examples.Add(detail); }
		}

		public int CheckFailures { get; private set; }

		// Compare every layout this file reads against the managed API on the live objects.  A failure of a
		// structural check (module, vtables, instance ids, TimeManager) turns the native section off.
		public void Check(List<GameObject> gos, List<Component> comps)
		{
			if (!Enabled) return;
			_tally.Clear();
			try
			{
				long tm = TimeManagerPtr(), dcm = DelayedCallManagerPtr();
				Expect("vtable.TimeManager", P(tm) == _base + VT_TimeManager, $"0x{P(tm):x}");
				Expect("vtable.DelayedCallManager", P(dcm) == _base + VT_DelayedCallManager, $"0x{P(dcm):x}");
				for (int i = 0; i < 4; i++)
				{
					long bm = P(_base + RVA_BehaviourManagers[i]);
					Expect("vtable.BehaviourManager." + BehaviourManagerNames[i], bm != 0 && P(bm) == _base + VT_BehaviourManagers[i], $"0x{bm:x}");
				}
				Expect("time.frameCount", I64(tm + TM_FrameCount) == Time.frameCount, $"{I64(tm + TM_FrameCount)} vs {Time.frameCount}");
				Expect("time.timeScale", FloatToBits(F32(tm + TM_TimeScale)) == FloatToBits(Time.timeScale), $"{F32(tm + TM_TimeScale)} vs {Time.timeScale}");
				Expect("time.fixedDeltaTime", FloatToBits(F32(tm + TM_Fixed + 0x18)) == FloatToBits(Time.fixedDeltaTime), $"{F32(tm + TM_Fixed + 0x18)}");
				Expect("time.captureDeltaTime", FloatToBits(F32(tm + TM_CaptureDeltaTime)) == FloatToBits(Time.captureDeltaTime), $"{F32(tm + TM_CaptureDeltaTime)}");
				Expect("time.time", FloatToBits((float)F64(tm + TM_Active)) == FloatToBits(Time.time), $"{F64(tm + TM_Active)} vs {Time.time}");   // Time_Get_Custom_PropTime UP!0x18091ed70
				Expect("time.fixedTime", FloatToBits((float)F64(tm + TM_Fixed)) == FloatToBits(Time.fixedTime), $"{F64(tm + TM_Fixed)} vs {Time.fixedTime}");
				Expect("vtable.PhysicsManager2D", P(_pm) == _base + VT_PhysicsManager2D, $"0x{P(_pm):x}");
				var distinct = new HashSet<int>();
				for (int i = 0; i < 5; i++)
					Expect("physics.changeHandle", _pmHandles[i] >= 0 && _pmHandles[i] < 64 && distinct.Add(_pmHandles[i]), $"handle {i} = {_pmHandles[i]}");
				Expect("physics.handlesInUse", ((ulong)I64(_tcd + TCD_InUse) & _physMask) == _physMask,
					$"in use 0x{I64(_tcd + TCD_InUse):x}, physics 0x{_physMask:x}");
				Expect("time.deltaTime", FloatToBits(F32(tm + TM_Active + 0x18)) == FloatToBits(Time.deltaTime), $"{F32(tm + TM_Active + 0x18)} vs {Time.deltaTime}");   // UP!0x18091f0a0

				foreach (var go in gos)
				{
					long p = Ptr(go);
					if (p == 0) continue;
					Expect("go.instanceID", Iid(p) == go.GetInstanceID(), go.name);
					Expect("go.layer", I32(p + GO_Layer) == go.layer, go.name);
					Expect("go.activeSelf", (U8(p + GO_IsActive) != 0) == go.activeSelf, go.name);
					var t = go.transform;
					long tp = Ptr(t), th = P(tp + TR_Access);
					long idx = (uint)I32(tp + TR_Access + 8);
					Expect("transform.hierarchyPointer", P(P(th + TH_Pointers) + 8 * idx) == tp, go.name);
					long trs = P(th + TH_Local) + TRS_Size * idx;
					Expect("transform.localPosition", Same3(trs, t.localPosition), go.name);
					var q = t.localRotation;
					Expect("transform.localRotation", Same4(trs + 0x10, q.x, q.y, q.z, q.w), go.name);
					Expect("transform.localScale", Same3(trs + 0x20, t.localScale), go.name);
					// a hierarchy with pending changes is queued in the dispatch at its changeDispatchIndex; -1 = not
					// queued (TransformChangeDispatch.c:699-701 queues on change, :1070-1081 dequeues to -1)
					int di = I32(th + TH_DispatchIndex);
					Expect("transform.dispatchIndex", (di == -1 || di >= 0 && di < I64(_tcd + TCD_Hierarchies + DA_Size)
						&& P(P(_tcd + TCD_Hierarchies + DA_Ptr) + 8L * di) == th) && idx < (uint)I32(th + TH_Capacity), go.name + $" {di}");
				}
				var scenes = new HashSet<long>();
				foreach (var c in comps)
				{
					long p = Ptr(c);
					if (p == 0) continue;
					string nm = c.GetType().Name + "@" + c.gameObject.name;
					Expect("component.instanceID", Iid(p) == c.GetInstanceID(), nm);
					Expect("component.gameObject", P(p + COMP_GameObject) == Ptr(c.gameObject), nm);
					if (c is Behaviour b)
					{
						Expect("behaviour.enabled", (U8(p + BEH_Enabled) != 0) == b.enabled, nm);
						// isActiveAndEnabled reads m_IsAdded itself (Behaviour_Get_Custom_PropIsActiveAndEnabled
						// UP!0x180977a80), so this pins the offset only; the next check is the independent one.
						Expect("behaviour.isAddedOffset", (U8(p + BEH_IsAdded) != 0) == b.isActiveAndEnabled, nm);
						bool destroying = b is MonoBehaviour && U8(p + MB_IsDestroying) != 0;
						if (!destroying)
							Expect("behaviour.isAdded=enabled&&activeInHierarchy", (U8(p + BEH_IsAdded) != 0) == (b.enabled && b.gameObject.activeInHierarchy), nm);
					}
					if (c is MonoBehaviour mb && mb.isActiveAndEnabled)
					{
						Expect("mono.addedToManager", U8(p + MB_AddedToManager) != 0, nm);
						Expect("mono.didAwake", U8(p + MB_DidAwake) != 0, nm);
						foreach (long co in CoroutineList(p)) Expect("coroutine.behaviour", P(co + CO_Behaviour) == p, nm);
					}
					if (c is Rigidbody2D rb)
					{
						Expect("rb.gravityScale", FloatToBits(F32(p + RB_GravityScale)) == FloatToBits(rb.gravityScale), nm);
						Expect("rb.simulated", (U8(p + RB_Simulated) != 0) == rb.simulated, nm);
						Expect("rb.bodyType", I32(p + RB_BodyType) == (int)rb.bodyType, nm);
						long body = P(p + RB_Body);
						if (body != 0 && rb.simulated)
						{
							var bb = Bytes(body, BODY_Size);
							Expect("b2body.userData", BitConverter.ToInt64(bb, 0xc0) == p, nm);
							// not rb.position: it auto-syncs transforms (Rigidbody2D::GetPosition UP!0x180c12290)
							if (rb.bodyType != RigidbodyType2D.Static)
							{
								var wc = rb.worldCenterOfMass;   // m_sweep.c (Rigidbody2D::GetWorldCenterOfMass UP!0x180c12860)
								Expect("b2body.sweepC", FB(bb, 0x2c) == StateWriter.FloatBits(wc.x) && FB(bb, 0x30) == StateWriter.FloatBits(wc.y), nm);
								// m_angularVelocity * 57.29578 (Rigidbody2D::GetAngularVelocity UP!0x180c11a50)
								Expect("b2body.angularVelocity", StateWriter.FloatBits(BitConverter.ToSingle(bb, 0x48) * 57.29578f) == StateWriter.FloatBits(rb.angularVelocity), nm);
							}
							Expect("b2body.velocity", FB(bb, 0x40) == StateWriter.FloatBits(rb.velocity.x) && FB(bb, 0x44) == StateWriter.FloatBits(rb.velocity.y), nm);
							Expect("b2body.gravityScale", FB(bb, 0xb0) == StateWriter.FloatBits(rb.gravityScale), nm);
							int b2type = BitConverter.ToInt32(bb, 0);
							int want = rb.bodyType == RigidbodyType2D.Dynamic ? 2 : rb.bodyType == RigidbodyType2D.Kinematic ? 1 : 0;
							Expect("b2body.type", b2type == want, nm + $" {b2type}");
							Expect("b2body.awake", ((BitConverter.ToUInt16(bb, 4) & 2) != 0) == rb.IsAwake(), nm);
						}
						long sc = P(p + RB_Scene);
						if (sc != 0) scenes.Add(sc);
					}
					if (c is Collider2D col)
					{
						Expect("col.isTrigger", (U8(p + COL_IsTrigger) != 0) == col.isTrigger, nm);
						Expect("col.offset", Same2(p + COL_Offset, col.offset), nm);
						Expect("col.density", FloatToBits(F32(p + COL_Density)) == FloatToBits(col.density), nm);
						if (col is BoxCollider2D box)
						{
							Expect("box.size", Same2(p + BOX_Size, box.size), nm);
							Expect("box.edgeRadius", FloatToBits(F32(p + BOX_EdgeRadius)) == FloatToBits(box.edgeRadius), nm);
						}
						else if (col is CircleCollider2D cc) Expect("circle.radius", FloatToBits(F32(p + CIRCLE_Radius)) == FloatToBits(cc.radius), nm);
						else if (col is EdgeCollider2D ec) Expect("edge.edgeRadius", FloatToBits(F32(p + EDGE_EdgeRadius)) == FloatToBits(ec.edgeRadius), nm);
						int n = (int)I64(p + COL_Shapes + DA_Size);
						Expect("col.shapeCount", n == col.shapeCount, nm + $" {n} vs {col.shapeCount}");
						long arr = P(p + COL_Shapes + DA_Ptr);
						for (int i = 0; i < n && i < 64; i++)
						{
							long fx = P(arr + 8 * i);
							Expect("fixture.userData", P(fx + 0x50) == p, nm);
							Expect("fixture.isSensor", (U8(fx + 0x4a) != 0) == col.isTrigger, nm);
							int st = I32(P(fx + 0x28) + 8);
							Expect("fixture.shapeType", st >= 0 && st <= 4, nm + $" {st}");
						}
						long sc = P(p + COL_Scene);
						if (sc != 0) scenes.Add(sc);
					}
				}
				Expect("physics.oneScene", scenes.Count <= 1, $"{scenes.Count} PhysicsScene2D pointers");
				if (scenes.Count >= 1)
				{
					foreach (long s in scenes) { _scene = s; break; }
					CheckWorld(P(_scene + PS_World));
				}
				CheckQueues(dcm, comps);
			}
			catch (BadRead e) { Expect("readable", false, e.Message); }
			catch (Exception e) { Expect("exception", false, e.GetType().Name + ": " + e.Message); }

			int bad = 0;
			foreach (var kv in _tally)
			{
				bad += kv.Value.Bad;
				var o = Checks[kv.Key] as JObject ?? new JObject { ["ok"] = 0, ["bad"] = 0, ["examples"] = new JArray() };
				o["ok"] = (int)o["ok"] + kv.Value.Ok;
				o["bad"] = (int)o["bad"] + kv.Value.Bad;
				foreach (var x in kv.Value.Examples) if (((JArray)o["examples"]).Count < 4) ((JArray)o["examples"]).Add(x);
				Checks[kv.Key] = o;
			}
			CheckFailures += bad;
			string summary = $"layout check frame {Time.frameCount}: {_tally.Count} checks, {bad} mismatches";
			_log(summary);
			_w.Note(summary);
			foreach (var kv in _tally) if (kv.Value.Bad > 0) { _log($"  {kv.Key}: {kv.Value.Bad} bad, e.g. {kv.Value.Examples}"); _w.Note($"layout FAIL {kv.Key}: {kv.Value.Bad}"); }
			foreach (string structural in new[] { "vtable.TimeManager", "vtable.DelayedCallManager", "vtable.PhysicsManager2D", "physics.changeHandle", "transform.dispatchIndex", "component.instanceID", "go.instanceID", "time.frameCount", "readable", "exception" })
				if (_tally.TryGetValue(structural, out Tally t) && t.Bad > 0) { Disable("layout check failed: " + structural); break; }
		}

		private bool Same2(long a, Vector2 v) => I32(a) == FloatToBits(v.x) && I32(a + 4) == FloatToBits(v.y);
		private bool Same3(long a, Vector3 v) => Same2(a, v) && I32(a + 8) == FloatToBits(v.z);
		private bool Same4(long a, float x, float y, float z, float w) =>
			I32(a) == FloatToBits(x) && I32(a + 4) == FloatToBits(y) && I32(a + 8) == FloatToBits(z) && I32(a + 12) == FloatToBits(w);

		private void CheckWorld(long world)
		{
			int counted = 0;
			for (long b = P(world + W_BodyList); b != 0 && counted < 100000; b = P(b + 0x68)) counted++;
			Expect("world.bodyCount", counted == I32(world + W_BodyCount), $"{counted} vs {I32(world + W_BodyCount)}");
			long cm = world + W_CM;
			int nNon = (int)I64(cm + CM_NonTOI + DA_Size), nToi = (int)I64(cm + CM_TOI + DA_Size), listed = 0;
			for (long c = P(cm + CM_ContactList); c != 0 && listed < 1000000; c = P(c + 0x18))
			{
				listed++;
				int mi = I32(c + 0x124);
				bool inNon = mi >= 0 && mi < nNon && P(P(cm + CM_NonTOI) + 8L * mi) == c;
				bool inToi = mi >= 0 && mi < nToi && P(P(cm + CM_TOI) + 8L * mi) == c;
				Expect("contact.managerIndex", inNon ^ inToi, $"0x{c:x} idx {mi}");
			}
			Expect("contact.arraysCoverList", listed == nNon + nToi, $"{listed} vs {nNon}+{nToi}");
			int proxies = 0, walked = 0;
			for (long b = P(world + W_BodyList); b != 0 && walked < 1000000; b = P(b + 0x68))
				for (long f = P(b + 0x70); f != 0 && ++walked < 1000000; f = P(f + 0x18)) proxies += I32(f + 0x40);
			Expect("broadphase.proxyCount", proxies == I32(cm + BP_ProxyCount), $"{proxies} vs {I32(cm + BP_ProxyCount)}");
			long contacts = P(_scene + PS_Contacts);
			int nCol = (int)I64(contacts + PC_Collisions + DA_Size);
			long colArr = P(contacts + PC_Collisions + DA_Ptr);
			for (int i = 0; i < nCol && i < 100000; i++)
			{
				long rec = P(colArr + 8L * i);
				int ia = Iid(P(rec + 0x28)), ib = Iid(P(rec + 0x30)), st = I32(rec + 0x20);
				Expect("collision2d.colliderAFirst", ia < ib, $"{ia} {ib}");
				Expect("collision2d.state", st >= 1 && st <= 4, $"{st}");
			}
		}

		private void CheckQueues(long dcm, List<Component> comps)
		{
			var inUpdate = new HashSet<int>();
			long bm = P(_base + RVA_BehaviourManagers[0]);
			foreach (var bucket in Buckets(bm))
				foreach (var list in new[] { bucket.Value.Key, bucket.Value.Value })
					foreach (long beh in ListData(list)) inUpdate.Add(Iid(beh));
			foreach (var c in comps)
			{
				if (!(c is MonoBehaviour mb) || !mb.isActiveAndEnabled) continue;
				if (!DeclaresMessage(mb.GetType(), "Update")) continue;
				Expect("updateList.member", inUpdate.Contains(mb.GetInstanceID()), mb.GetType().Name + "@" + mb.gameObject.name);
			}
			double prev = double.NegativeInfinity;
			int n = 0;
			foreach (long node in TreeInOrder(dcm + DCM_Head, (long)I64(dcm + DCM_Size)))
			{
				double t = F64(node + TN_Value);
				Expect("delayedCall.sorted", t >= prev, $"{t} after {prev}");
				prev = t;
				long call = P(node + TN_Value + 0x20) - _base;
				Expect("delayedCall.knownCall", CallNames.ContainsKey(call), $"rva 0x{call:x}");
				n++;
			}
			Expect("delayedCall.size", n == I64(dcm + DCM_Size), $"{n} vs {I64(dcm + DCM_Size)}");
		}

		// Unity dispatches a message to the most-derived declaration (LifecycleRecorder.DeclOf).
		private static readonly Dictionary<Type, bool> _declUpdate = new Dictionary<Type, bool>();
		private static bool DeclaresMessage(Type t, string name)
		{
			if (_declUpdate.TryGetValue(t, out bool r)) return r;
			for (var bt = t; bt != null && bt != typeof(MonoBehaviour); bt = bt.BaseType)
			{
				var m = bt.GetMethod(name, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly, null, Type.EmptyTypes, null);
				if (m != null) { r = true; break; }
			}
			_declUpdate[t] = r;
			return r;
		}

		// ================================================================ containers
		// MSVC std::_Tree in-order walk (ascending key); `head` is the address of _Myhead.
		private IEnumerable<long> TreeInOrder(long headField, long size)
		{
			long head = P(headField);
			long node = P(head + TN_Left);   // leftmost
			long n = 0, guard = 0, limit = 4 * size + 64;
			while (node != head && n <= size)
			{
				yield return node;
				n++;
				long r = P(node + TN_Right);
				if (U8(r + TN_IsNil) == 0)
				{
					node = r;
					for (long l = P(node + TN_Left); U8(l + TN_IsNil) == 0; l = P(node + TN_Left))
					{
						node = l;
						if (++guard > limit) throw new BadRead("tree walk does not terminate");
					}
				}
				else
				{
					long p = P(node + TN_Parent);
					while (U8(p + TN_IsNil) == 0 && node == P(p + TN_Right))
					{
						node = p; p = P(p + TN_Parent);
						if (++guard > limit) throw new BadRead("tree walk does not terminate");
					}
					node = p;
				}
			}
			if (n > size) throw new BadRead("tree walk longer than its size");
		}

		// BaseBehaviourManager buckets: execution order -> (active list, add list)
		private IEnumerable<KeyValuePair<int, KeyValuePair<long, long>>> Buckets(long bm)
		{
			foreach (long node in TreeInOrder(bm + BM_Head, I64(bm + BM_Size)))
				yield return new KeyValuePair<int, KeyValuePair<long, long>>(I32(node + BMN_Key),
					new KeyValuePair<long, long>(P(node + BMN_Active), P(node + BMN_Add)));
		}

		// List<ListNode<T>>: root ListElement{prev,next} at `list`; each node's m_Data at +0x10.
		private List<long> ListData(long list)
		{
			var r = new List<long>();
			if (list == 0) return r;
			for (long n = P(list + 8); n != list; n = P(n + 8))
			{
				r.Add(P(n + LN_Data));
				if (r.Count > 200000) throw new BadRead("behaviour list does not close");
			}
			return r;
		}

		private List<long> CoroutineList(long mb)
		{
			var r = new List<long>();
			long root = mb + MB_ActiveCoroutines;
			for (long n = P(root + 8); n != root; n = P(n + 8))
			{
				r.Add(n);
				if (r.Count > 10000) throw new BadRead("coroutine list does not close");
			}
			return r;
		}

		private long TimeManagerPtr() => P(_base + RVA_gContext + 8 * CTX_TimeManager);
		private long DelayedCallManagerPtr() => P(_base + RVA_gContext + 8 * CTX_DelayedCallManager);

		private long[] Iids(List<long> objs)
		{
			var a = new long[objs.Count];
			for (int i = 0; i < a.Length; i++) a[i] = Iid(objs[i]);
			return a;
		}

		// ================================================================ per-behaviour native state
		// Native flags of one Behaviour (and, for a MonoBehaviour, its running coroutines) on its entity.
		public void PutBehaviour(StateWriter.Ent e, Behaviour b)
		{
			// n.* fields are this frame's reading only while n.valid; once the section is off they keep stale values
			if (!Enabled) { _w.B(e, "n.valid", false); return; }
			try { PutBehaviourNative(e, b); }
			catch (BadRead ex) { Disable("behaviour: " + ex.Message); }
		}

		private static readonly StateRecorder.Schema BehSchema = new StateRecorder.Schema("b:n.valid", "b:n.enabled"),
			MonoSchema = new StateRecorder.Schema("b:n.didAwake", "b:n.didStart", "b:n.isDestroying", "b:n.addedToManager",
				"b:n.inUpdateList", "b:n.inFixedList", "b:n.inLateList", "E:n.coroutines"),
			TransformSchema = new StateRecorder.Schema("b:n.valid", "i:n.dispatchIndex", "i:n.index", "l:n.physChanged", "l:n.physInterest");
		private long[] _coBuf = new long[16];

		private void PutBehaviourNative(StateWriter.Ent e, Behaviour b)
		{
			var s = StateRecorder.Slots(_w, e.Cls, BehSchema);
			long p = Ptr(b);
			if (p == 0) { _w.Raw(e, s[0], 0); return; }
			_w.Raw(e, s[0], 1);
			_w.Raw(e, s[1], U8(p + BEH_Enabled) != 0 ? 1 : 0);
			if (b is Collider2D)
			{
				// the body-relative shape matrix and body scale that SyncTransforms compares to decide a recreate
				// (native-physics2d.md section 2), and the fixtures in m_Shapes order
				var rt = new long[16];
				for (int i = 0; i < 16; i++) rt[i] = I32(p + COL_RelativeTransform + 4 * i) & 0xffffffffL;
				_w.List(e, "n.relativeTransform", 'F', rt);
				_w.List(e, "n.rigidbodyScale", 'F', new[] { I32(p + COL_RigidbodyScale) & 0xffffffffL,
					I32(p + COL_RigidbodyScale + 4) & 0xffffffffL, I32(p + COL_RigidbodyScale + 8) & 0xffffffffL });
				_w.I(e, "n.errorState", I32(p + COL_ErrorState));
				// the fixtures themselves are b2Fixture entities (collider = this iid); a recreate is a fixture death + birth
				_w.I(e, "n.shapeCount", (int)I64(p + COL_Shapes + DA_Size));
				return;
			}
			if (!(b is MonoBehaviour)) return;
			var m = StateRecorder.Slots(_w, e.Cls, MonoSchema);
			_w.Raw(e, m[0], U8(p + MB_DidAwake) != 0 ? 1 : 0);
			_w.Raw(e, m[1], U8(p + MB_DidStart) != 0 ? 1 : 0);
			_w.Raw(e, m[2], U8(p + MB_IsDestroying) != 0 ? 1 : 0);
			_w.Raw(e, m[3], U8(p + MB_AddedToManager) != 0 ? 1 : 0);
			_w.Raw(e, m[4], P(p + MB_UpdateNode) != 0 ? 1 : 0);
			_w.Raw(e, m[5], P(p + MB_FixedNode) != 0 ? 1 : 0);
			_w.Raw(e, m[6], P(p + MB_LateNode) != 0 ? 1 : 0);
			long root = p + MB_ActiveCoroutines;
			int nco = 0;
			for (long n = P(root + 8); n != root; n = P(n + 8))
			{
				if (nco == _coBuf.Length) Array.Resize(ref _coBuf, nco * 2);
				_coBuf[nco++] = n;
				if (nco > 10000) throw new BadRead("coroutine list does not close");
			}
			if (nco == 0) { _w.RawList(e, m[7], _coBuf, 0); return; }
			var cos = new long[nco];
			Array.Copy(_coBuf, cos, nco);
			for (int i = 0; i < nco; i++) cos[i] = CoroutineEid(cos[i], e.Eid);
			_w.RawList(e, m[7], cos, nco);
		}

		private int CoroutineEid(long co, int ownerEid)
		{
			long key = StateWriter.Key(NS_COROUTINE, co);
			if (_w.VisitedThisFrame(key)) return _w.EidOf(key);
			long beh = P(co + CO_Behaviour);
			ulong h = (ulong)I64(co + CO_EnumGCHandle);
			object iter = Enumerator(co, beh, h);
			string name = iter == null ? "" : iter.GetType().FullName;
			var e = _w.Visit(key, _w.Class("Coroutine"), beh ^ (long)(uint)h, ownerEid, name);
			if (e == null) return _w.EidOf(key);
			_w.O(e, "behaviour", Iid(beh));
			_w.S(e, "iterator.type", name);
			_w.I(e, "refCount", I32(co + CO_RefCount));
			_w.B(e, "doneRunning", U8(co + CO_Done) != 0);
			_w.B(e, "isIEnumerator", U8(co + CO_IsIEnumerator) != 0);
			_w.B(e, "asyncOperation", P(co + CO_AsyncOp) != 0);
			long cwf = P(co + CO_ContinueWhenFinished), wf = P(co + CO_WaitingFor);
			_w.E(e, "continueWhenFinished", cwf == 0 ? 0 : CoroutineEid(cwf, ownerEid));
			_w.E(e, "waitingFor", wf == 0 ? 0 : CoroutineEid(wf, ownerEid));
			_w.E(e, "iterator", iter == null || Plain == null ? 0 : Plain(iter, e.Eid, "iterator", 0));
			_w.Commit(e);
			return e.Eid;
		}

		// The coroutine's enumerator through its GC handle.  mono_gchandle_get_target is called only on a handle of
		// the shape MonoBehaviour gives it (ScriptingGCHandle::AcquireStrong, MonoBehaviour.c:2711): the coroutine is
		// a node of its owner's m_ActiveCoroutines list, the handle is strong and 32-bit, and its cached object and
		// that object's vtable are readable (Coroutine::Run uses m_Object itself for a strong handle,
		// Coroutine.c:449-452).  A handle that resolves to anything but an IEnumerator turns the native section off.
		private object Enumerator(long co, long beh, ulong h)
		{
			if (beh == 0 || h == 0 || h > uint.MaxValue) return null;
			if (!_coroutinesOf.TryGetValue(beh, out HashSet<long> mine)) _coroutinesOf[beh] = mine = new HashSet<long>(CoroutineList(beh));
			if (!mine.Contains(co)) return null;
			if (I32(co + CO_EnumGCHandle + GCH_Weakness) != GCHANDLE_STRONG) return null;
			long obj = P(co + CO_EnumGCHandle + GCH_Object);
			if (obj == 0) return null;
			P(P(obj));   // MonoObject.vtable -> MonoVTable.klass: both pages readable, else BadRead
			object iter;
			try { iter = GCHandle.FromIntPtr(new IntPtr((int)(uint)h)).Target; }
			catch (Exception ex) { throw new BadRead("coroutine GC handle: " + ex.Message); }
			if (iter != null && !(iter is System.Collections.IEnumerator)) throw new BadRead("coroutine GC handle resolves to " + iter.GetType().FullName);
			return iter;
		}

		// A Transform's pending Transform-to-Box2D sync: its TransformChangeDispatch bits for the five physics
		// systems (systemChanged[index] & the physics mask, which PhysicsManager2D::SyncTransforms UP!0x180bebc70
		// drains through GetAndClearChangedTransformsForMultipleSystems), and the physics bits it is watched for
		// (systemInterested[index]).  A moved-and-restored transform keeps its bit.  The drain walks the dispatch's
		// hierarchies in order (n.dispatchIndex), then the transforms of each (n.index).
		public void PutTransform(StateWriter.Ent e, Transform t)
		{
			if (!Enabled) { _w.B(e, "n.valid", false); return; }
			try
			{
				var s = StateRecorder.Slots(_w, e.Cls, TransformSchema);
				long tp = Ptr(t);
				if (tp == 0) { _w.Raw(e, s[0], 0); return; }
				long th = P(tp + TR_Access);
				long idx = (uint)I32(tp + TR_Access + 8);
				_w.Raw(e, s[0], 1);
				_w.Raw(e, s[1], I32(th + TH_DispatchIndex));
				_w.Raw(e, s[2], (int)idx);
				_w.Raw(e, s[3], (long)((ulong)I64(P(th + TH_SystemChanged) + 8 * idx) & _physMask));
				_w.Raw(e, s[4], (long)((ulong)I64(P(th + TH_SystemInterested) + 8 * idx) & _physMask));
			}
			catch (BadRead ex) { Disable("transform: " + ex.Message); }
		}

		// Rigidbody2D.position and .rotation as Rigidbody2D::GetPosition / ::GetRotation compute them
		// (UP!0x180c12290, UP!0x180c126b0), without their AutoSyncTransforms: m_Body->m_xf.p and
		// m_sweep.a * 57.29578; with no body, the Transform's position and eulerAngles.z (GetRotation uses
		// ZAngleFromRot there).
		public void PutRigidbodyPose(StateWriter.Ent e, Rigidbody2D rb)
		{
			long body = 0;
			byte[] bb = null;
			if (Enabled)
			{
				try { long p = Ptr(rb); if (p != 0) body = P(p + RB_Body); if (body != 0) bb = Bytes(body, 0x40); }
				catch (BadRead ex) { Disable("rigidbody pose: " + ex.Message); bb = null; }
			}
			_w.B(e, "hasBody", bb != null);
			if (bb != null)
			{
				_w.F(e, "position.x", BitConverter.ToSingle(bb, 0xc));
				_w.F(e, "position.y", BitConverter.ToSingle(bb, 0x10));
				_w.F(e, "rotation", BitConverter.ToSingle(bb, 0x38) * 57.29578f);
			}
			else
			{
				var t = rb.transform;
				var pos = t.position;
				_w.F(e, "position.x", pos.x);
				_w.F(e, "position.y", pos.y);
				_w.F(e, "rotation", t.eulerAngles.z);
			}
		}

		// ================================================================ engine singletons
		public void CaptureEngine(int envEid)
		{
			if (!Enabled) return;
			string section = "time";
			try
			{
				CaptureTime();
				section = "behaviour managers"; CaptureBehaviourManagers();
				section = "delayed calls"; CaptureDelayedCalls();
				section = "physics manager"; CapturePhysicsManager();
				section = "physics"; CapturePhysics();
			}
			catch (BadRead e) { Disable($"{section}: {e.Message}"); }
			catch (Exception e) { Disable($"{section}: {e.GetType().Name}: {e.Message}"); }
		}

		private StateWriter.Ent Single(string cls, int id)
		{
			return _w.Visit(StateWriter.Key(NS_SINGLE, id), _w.Class(cls), 0, 0, cls);
		}

		private void CaptureTime()
		{
			long tm = TimeManagerPtr();
			var e = Single("TimeManager", 1);
			var b = Bytes(tm, 0x108);
			string[] hn = { "fixed", "dynamic", "active" };
			int[] ho = { TM_Fixed, TM_Dynamic, TM_Active };
			for (int i = 0; i < 3; i++)
			{
				_w.D(e, hn[i] + ".cur", BitConverter.ToDouble(b, ho[i]));
				_w.D(e, hn[i] + ".last", BitConverter.ToDouble(b, ho[i] + 8));
				_w.F(e, hn[i] + ".delta", BitConverter.ToSingle(b, ho[i] + 0x18));
			}
			_w.B(e, "firstFrameAfterReset", b[TM_FirstFrameAfterReset] != 0);
			_w.B(e, "firstFrameAfterPause", b[TM_FirstFrameAfterPause] != 0);
			_w.B(e, "firstFixedFrameAfterReset", b[TM_FirstFixedFrameAfterReset] != 0);
			_w.L(e, "frameCount", BitConverter.ToInt64(b, TM_FrameCount));
			_w.F(e, "captureDeltaTime", BitConverter.ToSingle(b, TM_CaptureDeltaTime));
			_w.D(e, "sceneLoadOffset", BitConverter.ToDouble(b, TM_SceneLoadOffset));
			_w.B(e, "useFixedTimeStep", b[TM_UseFixedTimeStep] != 0);
			_w.F(e, "timeScale", BitConverter.ToSingle(b, TM_TimeScale));
			_w.F(e, "maximumTimestep", BitConverter.ToSingle(b, TM_MaximumTimestep));
			_w.Commit(e);
		}

		// Per manager and execution-order bucket, the active list then the add list, as instance ids in list order.
		private void CaptureBehaviourManagers()
		{
			for (int i = 0; i < 4; i++)
			{
				long bm = P(_base + RVA_BehaviourManagers[i]);
				var e = Single("BehaviourManager", 10 + i);
				_w.S(e, "name", BehaviourManagerNames[i]);
				var keys = new List<long>();
				if (bm != 0)
					foreach (var bucket in Buckets(bm))
					{
						keys.Add(bucket.Key);
						_w.List(e, "q" + bucket.Key + ".active", 'O', Iids(ListData(bucket.Value.Key)));
						_w.List(e, "q" + bucket.Key + ".add", 'O', Iids(ListData(bucket.Value.Value)));
					}
				_w.List(e, "orders", 'I', keys.ToArray());
				_w.Commit(e);
			}
		}

		// The DelayedCallManager queue in multiset order (ascending key, FIFO within a key): Start calls,
		// coroutine resumes, Destroy(obj, t) and Invoke.
		private void CaptureDelayedCalls()
		{
			long dcm = DelayedCallManagerPtr();
			var q = Single("DelayedCallManager", 20);
			_w.I(q, "timeStamp", I32(dcm + DCM_TimeStamp));
			var cls = _w.Class("DelayedCall");
			var order = new List<long>();
			foreach (long node in TreeInOrder(dcm + DCM_Head, I64(dcm + DCM_Size)))
			{
				var b = Bytes(node + TN_Value, 0x40);
				long call = BitConverter.ToInt64(b, 0x20) - _base, cleanup = BitConverter.ToInt64(b, 0x28), user = BitConverter.ToInt64(b, 0x18);
				int obj = BitConverter.ToInt32(b, 0x30);
				long key = StateWriter.Key(NS_CALL, node);
				var e = _w.Visit(key, cls, call ^ ((long)obj << 32) ^ user, q.Eid, CallName(call));
				if (e == null) continue;
				_w.D(e, "time", BitConverter.ToDouble(b, 0));
				_w.L(e, "frame", BitConverter.ToInt64(b, 8));
				_w.F(e, "repeatRate", BitConverter.ToSingle(b, 0x10));
				_w.B(e, "repeat", b[0x14] != 0);
				_w.S(e, "call", CallName(call));
				_w.S(e, "cleanup", cleanup == 0 ? "" : CallName(cleanup - _base));
				_w.O(e, "object", obj);
				_w.I(e, "mode", BitConverter.ToInt32(b, 0x34));
				_w.I(e, "timeStamp", BitConverter.ToInt32(b, 0x38));
				_w.E(e, "coroutine", call == RVA_ContinueCoroutine && user != 0 ? CoroutineEid(user, e.Eid) : 0);
				_w.S(e, "invoke", call == RVA_ForwardInvokeDelayed && user != 0 ? CString(user, 256) : "");
				_w.Commit(e);
				order.Add(e.Eid);
			}
			_w.List(q, "queue", 'E', order.ToArray());
			_w.Commit(q);
		}

		private static string CallName(long rva) => CallNames.TryGetValue(rva, out string n) ? n : "rva:0x" + rva.ToString("x");

		private string CString(long a, int max)
		{
			var sb = new System.Text.StringBuilder();
			for (int i = 0; i < max; i++) { byte c = U8(a + i); if (c == 0) break; sb.Append((char)c); }
			return sb.ToString();
		}

		// ================================================================ physics
		// The change-system handles (bit positions in the Transforms' n.physChanged / n.physInterest) and the
		// dispatch's combined changed mask restricted to the physics bits.
		private void CapturePhysicsManager()
		{
			var e = Single("PhysicsManager2D", 33);
			string[] names = { "rigidbodyT", "rigidbodyR", "rigidbodyS", "rigidbodyAnim", "colliderTRS", "rigidbodyParentHierarchy", "colliderParentHierarchy" };
			for (int i = 0; i < names.Length; i++) _w.I(e, "handle." + names[i], _pmHandles[i]);
			_w.L(e, "physMask", (long)_physMask);
			_w.L(e, "dispatch.combinedChanged", (long)((ulong)I64(_tcd + TCD_Combined) & _physMask));
			_w.I(e, "dispatch.hierarchies", (int)I64(_tcd + TCD_Hierarchies + DA_Size));
			_w.Commit(e);
		}

		public void SetScene(Rigidbody2D rb)
		{
			if (!Enabled || rb == null) return;
			try { long p = Ptr(rb); if (p != 0) _scene = P(p + RB_Scene); }
			catch (BadRead e) { Disable("scene pointer: " + e.Message); }
		}

		private void CapturePhysics()
		{
			if (_scene == 0) return;
			long world = P(_scene + PS_World);
			var ps = Single("PhysicsScene2D", 30);
			_w.I(ps, "handle", I32(_scene + PS_Handle));
			_w.D(ps, "lastSimulationTime", F64(_scene + PS_LastSimTime));
			_w.F(ps, "lastSimulationDelta", F32(_scene + PS_LastSimDelta));
			_w.B(ps, "runningSimulationStep", U8(_scene + PS_Running) != 0);
			_w.B(ps, "rigidbodyHierarchyChanged", U8(_scene + PS_HierarchyChanged) != 0);
			_w.I(ps, "movementStates", (int)I64(_scene + PS_MoveStates + DA_Size));

			_proxyOwner.Clear(); _proxyChild.Clear(); _bodyEnt.Clear();
			long ground = P(_scene + PS_Ground);
			var bodyOrder = new List<long>();
			var bodies = new List<long>();
			for (long b = P(world + W_BodyList); b != 0; b = P(b + 0x68))
			{
				bodies.Add(b);
				if (bodies.Count > 200000) throw new BadRead("body list does not close");
			}
			var bodyEid = new Dictionary<long, int>();
			foreach (long b in bodies) bodyEid[b] = BodyEntity(b, b == ground);
			foreach (long b in bodies) bodyOrder.Add(bodyEid[b]);

			var we = Single("b2World", 31);
			_w.I(we, "flags", I32(world + W_Flags));
			_w.I(we, "bodyCount", I32(world + W_BodyCount));
			_w.List(we, "bodyList", 'E', bodyOrder.ToArray());
			_w.List(we, "nonStaticBodies", 'E', BodyArray(world + W_NonStatic, bodyEid));
			_w.List(we, "staticBodies", 'E', BodyArray(world + W_Static, bodyEid));
			_w.F(we, "gravity.x", F32(world + W_Gravity)); _w.F(we, "gravity.y", F32(world + W_Gravity + 4));
			_w.B(we, "allowSleep", U8(world + W_AllowSleep) != 0);
			_w.F(we, "inv_dt0", F32(world + W_InvDt0));
			_w.B(we, "warmStarting", U8(world + W_WarmStarting) != 0);
			_w.B(we, "continuousPhysics", U8(world + W_Continuous) != 0);
			_w.B(we, "subStepping", U8(world + W_SubStepping) != 0);
			_w.B(we, "stepComplete", U8(world + W_StepComplete) != 0);
			_w.I(we, "discreteIslands", I32(world + W_DiscreteIslands));
			_w.I(we, "continuousIslands", I32(world + W_ContinuousIslands));

			long cm = world + W_CM;
			var contactOrder = new List<long>();
			var contactEid = new Dictionary<long, int>();
			for (long c = P(cm + CM_ContactList); c != 0; c = P(c + 0x18))
			{
				int eid = ContactEntity(c);
				contactEid[c] = eid;
				contactOrder.Add(eid);
				if (contactOrder.Count > 1000000) throw new BadRead("contact list does not close");
			}
			_w.List(we, "contactList", 'E', contactOrder.ToArray());
			_w.List(we, "contactsNonTOI", 'E', PtrArray(cm + CM_NonTOI, contactEid));
			_w.List(we, "contactsTOI", 'E', PtrArray(cm + CM_TOI, contactEid));
			// body contact edges, newest first (b2ContactManager prepends)
			foreach (long b in bodies)
			{
				var edges = new List<long>();
				for (long ed = P(b + 0x88); ed != 0; ed = P(ed + 0x18))
				{
					long c = P(ed + 8);
					edges.Add(contactEid.TryGetValue(c, out int id) ? id : 0);
					if (edges.Count > 100000) throw new BadRead("contact edge list does not close");
				}
				var ent = _bodyEnt[b];
				_w.List(ent, "contactEdges", 'E', edges.ToArray());
				_w.Commit(ent);
			}
			CaptureBroadphase(we, cm);
			_w.Commit(we);
			CaptureCollisions(P(_scene + PS_Contacts), contactEid);
			_w.Commit(ps);
		}

		private readonly Dictionary<long, StateWriter.Ent> _bodyEnt = new Dictionary<long, StateWriter.Ent>();

		private int BodyEntity(long b, bool ground)
		{
			var bb = Bytes(b, BODY_Size);
			long user = BitConverter.ToInt64(bb, 0xc0);
			int rbIid = user == 0 ? 0 : Iid(user);
			var bcls = _w.Class("b2Body");
			long bkey = StateWriter.Key(NS_BODY, b);
			var e = _w.Visit(bkey, bcls, user, 0, _w.Known(bkey, bcls, user) ? null : ground ? "ground" : "rb#" + rbIid);
			_bodyEnt[b] = e;
			_w.O(e, "rigidbody", rbIid);
			_w.B(e, "ground", ground);
			_w.I(e, "type", BitConverter.ToInt32(bb, 0));
			_w.I(e, "flags", BitConverter.ToUInt16(bb, 4));
			_w.I(e, "islandIndex", BitConverter.ToInt32(bb, 8));
			_w.List(e, "xf", 'F', new[] { FB(bb, 0xc), FB(bb, 0x10), FB(bb, 0x14), FB(bb, 0x18) });
			_w.List(e, "sweep", 'F', new[] { FB(bb, 0x1c), FB(bb, 0x20), FB(bb, 0x24), FB(bb, 0x28), FB(bb, 0x2c), FB(bb, 0x30),
				FB(bb, 0x34), FB(bb, 0x38), FB(bb, 0x3c) });
			_w.F(e, "v.x", BitConverter.ToSingle(bb, 0x40)); _w.F(e, "v.y", BitConverter.ToSingle(bb, 0x44));
			_w.F(e, "w", BitConverter.ToSingle(bb, 0x48));
			_w.F(e, "force.x", BitConverter.ToSingle(bb, 0x4c)); _w.F(e, "force.y", BitConverter.ToSingle(bb, 0x50));
			_w.F(e, "torque", BitConverter.ToSingle(bb, 0x54));
			_w.F(e, "mass", BitConverter.ToSingle(bb, 0x90)); _w.F(e, "invMass", BitConverter.ToSingle(bb, 0x94));
			_w.F(e, "axisConstraint.x", BitConverter.ToSingle(bb, 0x98)); _w.F(e, "axisConstraint.y", BitConverter.ToSingle(bb, 0x9c));
			_w.F(e, "I", BitConverter.ToSingle(bb, 0xa0)); _w.F(e, "invI", BitConverter.ToSingle(bb, 0xa4));
			_w.F(e, "linearDamping", BitConverter.ToSingle(bb, 0xa8)); _w.F(e, "angularDamping", BitConverter.ToSingle(bb, 0xac));
			_w.F(e, "gravityScale", BitConverter.ToSingle(bb, 0xb0));
			_w.F(e, "sleepTime", BitConverter.ToSingle(bb, 0xb4));
			_w.I(e, "worldIndex", BitConverter.ToInt32(bb, 0xb8));
			if (user != 0)
			{
				var mb = Bytes(user + RB_Movement, 0x80);
				_w.B(e, "rb.linearMove", mb[0x7d] != 0);
				_w.B(e, "rb.angularMove", mb[0x7e] != 0);
				_w.B(e, "rb.interpolating", mb[0x7c] != 0);
				_w.F(e, "rb.linearTarget.x", BitConverter.ToSingle(mb, 0x70)); _w.F(e, "rb.linearTarget.y", BitConverter.ToSingle(mb, 0x74));
				_w.F(e, "rb.angularTarget", BitConverter.ToSingle(mb, 0x78));
				_w.I(e, "rb.movementIndex", BitConverter.ToInt32(mb, 0));
				_w.O(e, "rb.parentDrivenBy", Iid(P(user + RB_ParentDrivenBy)));
			}
			var fixtures = new List<long>();
			for (long f = BitConverter.ToInt64(bb, 0x70); f != 0; f = P(f + 0x18))
			{
				fixtures.Add(FixtureEntity(f, e.Eid));
				if (fixtures.Count > 100000) throw new BadRead("fixture list does not close");
			}
			_w.List(e, "fixtures", 'E', fixtures.ToArray());
			return e.Eid;   // committed by CapturePhysics after the contact edges
		}

		private int FixtureEntity(long f, int bodyEid)
		{
			var fb = Bytes(f, FIX_Size);
			long user = BitConverter.ToInt64(fb, 0x50), shape = BitConverter.ToInt64(fb, 0x28);
			int colIid = user == 0 ? 0 : Iid(user);
			var fcls = _w.Class("b2Fixture");
			long fkey = StateWriter.Key(NS_FIXTURE, f);
			var e = _w.Visit(fkey, fcls, user ^ (shape << 1), bodyEid, _w.Known(fkey, fcls, user ^ (shape << 1)) ? null : "col#" + colIid);
			if (e == null) return _w.EidOf(StateWriter.Key(NS_FIXTURE, f));
			_w.O(e, "collider", colIid);
			_w.List(e, "massData", 'F', new[] { FB(fb, 0), FB(fb, 4), FB(fb, 8), FB(fb, 0xc), FB(fb, 0x10) });
			_w.F(e, "density", BitConverter.ToSingle(fb, 0x14));
			_w.F(e, "friction", BitConverter.ToSingle(fb, 0x30));
			_w.F(e, "restitution", BitConverter.ToSingle(fb, 0x34));
			_w.List(e, "filter", 'I', new long[] { BitConverter.ToUInt16(fb, 0x44), BitConverter.ToUInt16(fb, 0x46), BitConverter.ToInt16(fb, 0x48) });
			_w.B(e, "isSensor", fb[0x4a] != 0);
			// shape
			int type = I32(shape + 8);
			_w.I(e, "shape.type", type);
			_w.F(e, "shape.radius", F32(shape + 0xc));
			var geo = new List<long>();
			switch (type)
			{
				case 0: geo.Add(I32(shape + 0x10) & 0xffffffffL); geo.Add(I32(shape + 0x14) & 0xffffffffL); break;       // circle p
				case 1: for (int o = 0x10; o < 0x30; o += 4) geo.Add(I32(shape + o) & 0xffffffffL); break;                // edge v1,v2,v0,v3
				case 2:
					{
						int n = Math.Min(I32(shape + 0x98), 8);
						geo.Add(I32(shape + 0x10) & 0xffffffffL); geo.Add(I32(shape + 0x14) & 0xffffffffL);                  // centroid
						for (int k = 0; k < 2 * n; k++) geo.Add(I32(shape + 0x18 + 4 * k) & 0xffffffffL);                  // vertices
						for (int k = 0; k < 2 * n; k++) geo.Add(I32(shape + 0x58 + 4 * k) & 0xffffffffL);                  // normals
						_w.I(e, "shape.count", n);
						break;
					}
				case 3:
					{
						int n = Math.Min(I32(shape + 0x18), 4096);
						long vs = P(shape + 0x10);
						for (int k = 0; k < 2 * n; k++) geo.Add(I32(vs + 4 * k) & 0xffffffffL);
						for (int o = 0x1c; o < 0x2c; o += 4) geo.Add(I32(shape + o) & 0xffffffffL);                          // prev, next
						_w.I(e, "shape.count", n);
						break;
					}
				case 4: for (int o = 0x10; o < 0x20; o += 4) geo.Add(I32(shape + o) & 0xffffffffL); break;                // capsule
			}
			_w.List(e, "shape.geometry", 'F', geo.ToArray());
			_w.List(e, "shape.flags", 'I', type == 1 ? new long[] { U8(shape + 0x30), U8(shape + 0x31) }
				: type == 3 ? new long[] { U8(shape + 0x2c), U8(shape + 0x2d) } : new long[0]);
			int np = BitConverter.ToInt32(fb, 0x40);
			long proxies = BitConverter.ToInt64(fb, 0x38);
			var ids = new List<long>(); var aabbs = new List<long>();
			for (int i = 0; i < np && i < 4096; i++)
			{
				long pr = proxies + (long)PROXY_Size * i;
				var pb = Bytes(pr, PROXY_Size);
				ids.Add(BitConverter.ToInt32(pb, 0x18)); ids.Add(BitConverter.ToInt32(pb, 0x1c));
				for (int k = 0; k < 4; k++) aabbs.Add(FB(pb, 4 * k));
				_proxyOwner[pr] = e.Eid; _proxyChild[pr] = BitConverter.ToInt32(pb, 0x18);
			}
			_w.List(e, "proxies", 'I', ids.ToArray());          // (childIndex, proxyId) pairs
			_w.List(e, "proxyAabbs", 'F', aabbs.ToArray());
			_w.Commit(e);
			return e.Eid;
		}

		private int ContactEntity(long c)
		{
			var cb = Bytes(c, CONTACT_Size);
			long fa = BitConverter.ToInt64(cb, 0x60), fbp = BitConverter.ToInt64(cb, 0x68);
			long sig = fa ^ (fbp << 1) ^ ((long)BitConverter.ToInt32(cb, 0x70) << 48) ^ ((long)BitConverter.ToInt32(cb, 0x74) << 56);
			long key = StateWriter.Key(NS_CONTACT, c);
			int ea = _w.EidOf(StateWriter.Key(NS_FIXTURE, fa)), eb = _w.EidOf(StateWriter.Key(NS_FIXTURE, fbp));
			var e = _w.Visit(key, _w.Class("b2Contact"), sig, 0, "");
			if (e == null) return _w.EidOf(key);
			_w.I(e, "flags", BitConverter.ToInt32(cb, 8));
			_w.E(e, "fixtureA", ea); _w.E(e, "fixtureB", eb);
			_w.I(e, "indexA", BitConverter.ToInt32(cb, 0x70)); _w.I(e, "indexB", BitConverter.ToInt32(cb, 0x74));
			_w.I(e, "islandIndexA", BitConverter.ToInt32(cb, 0x78)); _w.I(e, "islandIndexB", BitConverter.ToInt32(cb, 0x7c));
			var m = new List<long>(); var ids = new List<long>();
			for (int k = 0; k < 2; k++)
			{
				int o = 0x80 + 0x14 * k;
				m.Add(FB(cb, o)); m.Add(FB(cb, o + 4)); m.Add(FB(cb, o + 8)); m.Add(FB(cb, o + 12));
				ids.Add(BitConverter.ToUInt32(cb, o + 16));
			}
			for (int o = 0xa8; o < 0xb8; o += 4) m.Add(FB(cb, o));   // localNormal, localPoint
			m.Add(FB(cb, 0xc0)); m.Add(FB(cb, 0xc4));                   // radiusA, radiusB
			_w.List(e, "manifold", 'F', m.ToArray());
			_w.List(e, "manifold.ids", 'L', ids.ToArray());
			_w.I(e, "manifold.type", BitConverter.ToInt32(cb, 0xb8));
			_w.I(e, "manifold.pointCount", BitConverter.ToInt32(cb, 0xbc));
			_w.I(e, "toiCount", BitConverter.ToInt32(cb, 0x110));
			_w.F(e, "toi", BitConverter.ToSingle(cb, 0x114));
			_w.F(e, "friction", BitConverter.ToSingle(cb, 0x118));
			_w.F(e, "restitution", BitConverter.ToSingle(cb, 0x11c));
			_w.F(e, "tangentSpeed", BitConverter.ToSingle(cb, 0x120));
			_w.I(e, "managerIndex", BitConverter.ToInt32(cb, 0x124));
			_w.I(e, "userIndex", BitConverter.ToInt32(cb, 0x128));
			_w.Commit(e);
			return e.Eid;
		}

		private long[] BodyArray(long da, Dictionary<long, int> bodyEid)
		{
			int n = (int)I64(da + DA_Size);
			long arr = P(da + DA_Ptr);
			var r = new long[n];
			for (int i = 0; i < n; i++) { long b = P(arr + 8L * i); r[i] = bodyEid.TryGetValue(b, out int id) ? id : 0; }
			return r;
		}

		private long[] PtrArray(long da, Dictionary<long, int> eids)
		{
			int n = (int)I64(da + DA_Size);
			long arr = P(da + DA_Ptr);
			var r = new long[n];
			for (int i = 0; i < n; i++) { long p = P(arr + 8L * i); r[i] = eids.TryGetValue(p, out int id) ? id : 0; }
			return r;
		}

		// Broadphase: counters and move buffer on the world entity, one entity per dynamic-tree node slot
		// (free slots included: their next pointer is the free list).
		private void CaptureBroadphase(StateWriter.Ent we, long bp)
		{
			_w.I(we, "tree.root", I32(bp + TREE_Root));
			int count = I32(bp + TREE_NodeCount), cap = I32(bp + TREE_Capacity);
			_w.I(we, "tree.nodeCount", count);
			_w.I(we, "tree.nodeCapacity", cap);
			_w.I(we, "tree.freeList", I32(bp + TREE_FreeList));
			_w.I(we, "tree.path", I32(bp + TREE_Path));
			_w.I(we, "tree.insertionCount", I32(bp + TREE_Insertions));
			_w.I(we, "proxyCount", I32(bp + BP_ProxyCount));
			_w.I(we, "pairBuffer.size", (int)I64(bp + BP_PairBuffer + DA_Size));
			int nm = (int)I64(bp + BP_MoveBuffer + DA_Size);
			long mbuf = P(bp + BP_MoveBuffer + DA_Ptr);
			var moves = new long[Math.Max(0, nm)];
			for (int i = 0; i < moves.Length; i++) moves[i] = I32(mbuf + 4L * i);
			_w.List(we, "moveBuffer", 'I', moves);
			_w.I(we, "queryProxyId", I32(bp + BP_QueryProxyId));
			if (cap <= 0 || cap > 1 << 20) throw new BadRead("tree capacity " + cap);
			var nodes = Bytes(P(bp + TREE_Nodes), cap * NODE_Size);
			var cls = _w.Class("b2TreeNode");
			for (int i = 0; i < cap; i++)
			{
				int o = i * NODE_Size;
				long nkey = StateWriter.Key(NS_TREENODE, i);
				var e = _w.Visit(nkey, cls, 0, 0, _w.Known(nkey, cls, 0) ? null : "node#" + i);
				if (e == null) continue;
				long user = BitConverter.ToInt64(nodes, o + 0x10);
				_w.List(e, "aabb", 'F', new[] { FB(nodes, o), FB(nodes, o + 4), FB(nodes, o + 8), FB(nodes, o + 12) });
				_w.E(e, "fixture", user != 0 && _proxyOwner.TryGetValue(user, out int fe) ? fe : 0);
				_w.I(e, "childIndex", user != 0 && _proxyChild.TryGetValue(user, out int ci) ? ci : -1);
				_w.B(e, "hasUserData", user != 0);
				_w.I(e, "parentOrNext", BitConverter.ToInt32(nodes, o + 0x18));
				_w.I(e, "child1", BitConverter.ToInt32(nodes, o + 0x1c));
				_w.I(e, "child2", BitConverter.ToInt32(nodes, o + 0x20));
				_w.I(e, "height", BitConverter.ToInt32(nodes, o + 0x24));
				_w.Commit(e);
			}
		}

		// PhysicsContacts2D.m_Collisions in array order: the order of the step's OnTrigger*/OnCollision* reports.
		private void CaptureCollisions(long contacts, Dictionary<long, int> contactEid)
		{
			var pc = Single("PhysicsContacts2D", 32);
			_w.I(pc, "simulationId", I32(contacts + PC_SimulationId));
			int n = (int)I64(contacts + PC_Collisions + DA_Size);
			long arr = P(contacts + PC_Collisions + DA_Ptr);
			var order = new long[Math.Max(0, n)];
			var cls = _w.Class("Collision2D");
			for (int i = 0; i < order.Length; i++)
			{
				long rec = P(arr + 8L * i);
				var rb = Bytes(rec, C2D_Size);
				long ka = BitConverter.ToInt64(rb, 0x50), kb = BitConverter.ToInt64(rb, 0x58);
				var e = _w.Visit(StateWriter.Key(NS_COLLISION, rec), cls, ka ^ (kb << 1), pc.Eid, "");
				if (e == null) { order[i] = _w.EidOf(StateWriter.Key(NS_COLLISION, rec)); continue; }
				order[i] = e.Eid;
				_w.I(e, "state", BitConverter.ToInt32(rb, 0x20));
				_w.I(e, "contactCount", BitConverter.ToInt32(rb, 0x24));
				_w.O(e, "colliderA", Iid(BitConverter.ToInt64(rb, 0x28)));
				_w.O(e, "colliderB", Iid(BitConverter.ToInt64(rb, 0x30)));
				_w.O(e, "rigidbodyA", Iid(BitConverter.ToInt64(rb, 0x38)));
				_w.O(e, "rigidbodyB", Iid(BitConverter.ToInt64(rb, 0x40)));
				_w.O(e, "receivingCollider", Iid(BitConverter.ToInt64(rb, 0x48)));
				_w.O(e, "keyA", Iid(ka)); _w.O(e, "keyB", Iid(kb));
				_w.B(e, "enabled", rb[0x60] != 0);
				_w.B(e, "isTrigger", rb[0x61] != 0);
				_w.B(e, "flagForRecreate", rb[0x62] != 0);
				_w.B(e, "swappedReferences", rb[0x63] != 0);
				int nm = (int)BitConverter.ToInt64(rb, DA_Size);
				long marr = BitConverter.ToInt64(rb, DA_Ptr);
				var mc = new List<long>(); var mi = new List<long>(); var mf = new List<long>();
				for (int k = 0; k < nm && k < 256; k++)
				{
					var mb = Bytes(marr + (long)MF_Size * k, MF_Size);
					long c = BitConverter.ToInt64(mb, 8);
					mc.Add(c != 0 && contactEid.TryGetValue(c, out int ce) ? ce : 0);
					mi.Add(mb[0]); mi.Add(BitConverter.ToInt32(mb, 0x20)); mi.Add(BitConverter.ToInt32(mb, 0x24));
					mi.Add(BitConverter.ToInt32(mb, 0x28)); mi.Add(BitConverter.ToInt32(mb, 0x2c));
					for (int o = 0x30; o < 0x70; o += 4) mf.Add(FB(mb, o));
				}
				_w.List(e, "manifold.contacts", 'E', mc.ToArray());
				_w.List(e, "manifold.ints", 'I', mi.ToArray());       // per manifold: enabled, fixtureIndexA/B, simulationId, pointCount
				_w.List(e, "manifold.floats", 'F', mf.ToArray());     // per manifold: normal, points[2], relativeVelocity[2], separations, normal/tangent impulses
				_w.Commit(e);
			}
			_w.List(pc, "collisions", 'E', order);
			_w.Commit(pc);
		}
	}
}
