using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using UnityEngine;

namespace HKOracle.Record
{
	// The canonical path of a GameObject, the name the simulator gives it.  The simulator names an object by
	// its path in the scene dump, which DumpDriver takes at SceneReady, and never renames it.  The game renames
	// objects as it plays: ObjectPool.Spawn reparents a pooled clone out of _GameManager/GlobalPool
	// (ObjectPool.cs:488 `obj.parent = parent`) and Recycle puts it back (:250).  So the canonical path is the
	// object's path AT SceneReady (Snapshot); an object created after SceneReady is named by its nearest
	// snapshotted ancestor (or, for a pool clone, by the pool) plus its own dynamic path below that.
	public static class ScenePaths
	{
		private static readonly Dictionary<int, string> _snap = new Dictionary<int, string>();
		private static int _snapFrame = -1;
		private static FieldInfo _poolInstance, _poolSpawned;
		private static bool _poolResolved;

		// Every scene transform's path, once per frame however many recorders ask.
		public static int Snapshot()
		{
			if (_snapFrame == Time.frameCount && _snap.Count > 0) return _snap.Count;
			_snap.Clear();
			foreach (var t in Resources.FindObjectsOfTypeAll<Transform>())
			{
				if (t == null) continue;
				var go = t.gameObject;
				if (!go.scene.IsValid()) continue;   // prefabs / assets
				_snap[go.GetInstanceID()] = DynPath(t);
			}
			_snapFrame = Time.frameCount;
			return _snap.Count;
		}

		public static string DynPath(Transform t)
		{
			if (t == null) return "";
			var s = new StringBuilder(96);
			for (Transform cur = t; cur != null; cur = cur.parent)
			{
				if (s.Length > 0) s.Insert(0, '/');
				s.Insert(0, cur.name ?? "");
			}
			return s.ToString();
		}

		public static string Canon(GameObject go)
		{
			if (go == null) return "";
			string p;
			if (_snap.TryGetValue(go.GetInstanceID(), out p)) return p;
			string suffix = "";
			for (Transform cur = go.transform; cur != null; cur = cur.parent)
			{
				var cg = cur.gameObject;
				if (cur != go.transform && _snap.TryGetValue(cg.GetInstanceID(), out p))
					return suffix.Length > 0 ? p + "/" + suffix : p;
				if (IsPoolClone(cg))
				{
					string home = PoolHome();
					string me = home.Length > 0 ? home + "/" + cur.name : cur.name;
					return suffix.Length > 0 ? me + "/" + suffix : me;
				}
				suffix = suffix.Length > 0 ? cur.name + "/" + suffix : cur.name;
			}
			return suffix;
		}

		private static void ResolvePool()
		{
			if (_poolResolved) return;
			_poolResolved = true;
			_poolInstance = typeof(ObjectPool).GetField("_instance", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
			_poolSpawned = typeof(ObjectPool).GetField("spawnedObjects", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
		}

		private static bool IsPoolClone(GameObject go)
		{
			try
			{
				ResolvePool();
				var pool = _poolInstance?.GetValue(null) as ObjectPool;   // the backing field: the getter can create one
				if (pool == null || _poolSpawned == null) return false;
				var d = _poolSpawned.GetValue(pool) as Dictionary<GameObject, GameObject>;
				return d != null && d.ContainsKey(go);
			}
			catch { return false; }
		}

		private static string PoolHome()
		{
			try
			{
				ResolvePool();
				var pool = _poolInstance?.GetValue(null) as ObjectPool;
				return pool != null ? DynPath(pool.transform) : "";
			}
			catch { return ""; }
		}
	}
}
