using System;
using System.Collections.Generic;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using HKOracle.Env;

namespace HKOracle.Dump
{
	// hierarchy.json: every GameObject in the loaded scenes (root-down, depth-first, sibling order) with
	// transform (local + world), tag, layer, active flags, and every component (instance id, component index,
	// Behaviour.enabled / isActiveAndEnabled, PlayMakerFSM initialized / started): Unity types by name,
	// non-Unity MonoBehaviours with their primitive fields (ReflectionDumper.FieldDump), tk2dSpriteAnimator
	// with its clip library (emitted once, referenced afterwards), Collider2D/Rigidbody2D summaries.
	// Also: prefab ASSETS (scene.isLoaded == false) that carry a HealthManager, DamageHero,
	// EnemyHitEffectsUninfected, EnemyDeathEffects, Recoil, EventRegister or PlayMakerFSM, so pooled/spawned
	// objects and their component defaults are available.
	public static class HierarchyDumper
	{

		static JToken V(object o) => ReflectionDumper.Val(o, 1);
		static JArray Pts(Vector2[] p) { var a = new JArray(); if (p != null) foreach (var v in p) a.Add(new JArray(v.x, v.y)); return a; }
		static readonly string[] AssetInterest = { "HealthManager", "DamageHero", "EnemyHitEffectsUninfected", "EnemyDeathEffects",
			"Recoil", "EventRegister", "PlayMakerFSM", "DamageEnemies", "tk2dSpriteAnimator" };

		// `instanceID`: Object.GetInstanceID of the component (the lifecycle recorder's key); `index`: its place in
		// GetComponents, i.e. component order.
		static JObject Component(Component c, int index)
		{
			var ty = c.GetType();
			var cj = new JObject { ["type"] = ty.FullName, ["instanceID"] = c.GetInstanceID(), ["index"] = index };
			if (c is Behaviour b) { cj["enabled"] = b.enabled; cj["isActiveAndEnabled"] = b.isActiveAndEnabled; }
			if (c is Collider2D col)
			{
				cj["enabled"] = col.enabled; cj["density"] = col.density; cj["usedByEffector"] = col.usedByEffector;
				cj["sharedMaterial"] = ReflectionDumper.Material2D(col.sharedMaterial);
				cj["isTrigger"] = col.isTrigger; cj["offset"] = V(col.offset); cj["usedByComposite"] = col.usedByComposite;
				if (col is BoxCollider2D box) { cj["size"] = V(box.size); cj["edgeRadius"] = box.edgeRadius; }
				else if (col is CircleCollider2D cir) cj["radius"] = cir.radius;
				else if (col is PolygonCollider2D poly) { cj["pathCount"] = poly.pathCount; var paths = new JArray(); for (int i = 0; i < poly.pathCount; i++) paths.Add(Pts(poly.GetPath(i))); cj["paths"] = paths; }
				else if (col is EdgeCollider2D edge) { cj["edgeRadius"] = edge.edgeRadius; cj["points"] = Pts(edge.points); }
			}
			else if (c is Rigidbody2D rb)
			{
				foreach (var kv in ReflectionDumper.Rigidbody2DState(rb)) if (!cj.ContainsKey(kv.Key)) cj[kv.Key] = kv.Value;
			}
			else if (c is tk2dSpriteAnimator anim)
			{
				try { cj["animator"] = ReflectionDumper.Animator(anim); } catch (Exception ex) { cj["animator__error"] = ex.Message; }
			}
			else if (c is tk2dSprite spr)
			{
				try { cj["spriteId"] = spr.spriteId; cj["collection"] = spr.Collection == null ? null : spr.Collection.name; cj["scale"] = V(spr.scale); cj["color"] = V(spr.color); }
				catch (Exception ex) { cj["sprite__error"] = ex.Message; }
			}
			else if (c is PlayMakerFSM fsm)
			{
				try { cj["fsmName"] = fsm.FsmName; cj["fsmTemplate"] = fsm.FsmTemplate == null ? null : fsm.FsmTemplate.name; } catch { }
				// the readable half of "has it Awoken / Started": Fsm.Initialized is set by Awake's Init, Fsm.Started by Start
				try { cj["fsmInitialized"] = fsm.Fsm != null && fsm.Fsm.Initialized; cj["fsmStarted"] = fsm.Fsm != null && fsm.Fsm.Started; } catch { }
			}
			else if (!ty.FullName.StartsWith("UnityEngine.") && !ty.FullName.StartsWith("tk2d") && !ty.FullName.StartsWith("HutongGames."))
			{
				try { cj["fields"] = ReflectionDumper.FieldDump(c, typeof(MonoBehaviour)); }
				catch (Exception ex) { cj["fields__error"] = ex.Message; }
			}
			return cj;
		}

		static JObject GameObjectJson(GameObject go, string path, bool asset)
		{
			var t = go.transform;
			var o = new JObject
			{
				["path"] = path,
				["instanceID"] = go.GetInstanceID(),
				["activeSelf"] = go.activeSelf,
				["activeInHierarchy"] = go.activeInHierarchy,
				["tag"] = go.tag,
				["layer"] = go.layer,
				["isStatic"] = go.isStatic,
				["asset"] = asset,
				["localPosition"] = V(t.localPosition),
				["localEulerZ"] = t.localEulerAngles.z,
				["localScale"] = V(t.localScale),
				["position"] = V(t.position),
				["eulerZ"] = t.eulerAngles.z,
				["lossyScale"] = V(t.lossyScale),
				["siblingIndex"] = t.GetSiblingIndex(),
				["childCount"] = t.childCount,
			};
			var comps = new JArray();
			int index = 0;
			foreach (var c in go.GetComponents<Component>())
			{
				if (c == null) { comps.Add(new JObject { ["type"] = "(missing script)", ["index"] = index++ }); continue; }
				try { comps.Add(Component(c, index)); } catch (Exception ex) { comps.Add(new JObject { ["type"] = c.GetType().FullName, ["index"] = index, ["__error"] = ex.Message }); }
				index++;
			}
			o["components"] = comps;
			return o;
		}

		static void Walk(Transform t, string path, bool asset, JArray outArr, ref int count)
		{
			outArr.Add(GameObjectJson(t.gameObject, path, asset));
			count++;
			for (int i = 0; i < t.childCount; i++)
			{
				var ch = t.GetChild(i);
				Walk(ch, path + "/" + ch.name, asset, outArr, ref count);
			}
		}

		public static void Dump(string dir, string scene)
		{
			var arr = new JArray();
			int count = 0;
			var scenes = new JArray();
			for (int si = 0; si < UnityEngine.SceneManagement.SceneManager.sceneCount; si++)
			{
				var sc = UnityEngine.SceneManagement.SceneManager.GetSceneAt(si);
				if (!sc.isLoaded) continue;
				scenes.Add(sc.name);
				foreach (var root in sc.GetRootGameObjects())
					Walk(root.transform, root.name, false, arr, ref count);
			}
			// DontDestroyOnLoad objects are in no enumerable scene: reach them through every Transform's root
			var seenRoots = new HashSet<int>();
			foreach (var tr in Resources.FindObjectsOfTypeAll<Transform>())
			{
				if (tr == null) continue;
				var root = tr.root;
				// the DontDestroyOnLoad pseudo-scene reports isLoaded == true but is not enumerable via GetSceneAt
				if (root.gameObject.scene.name != "DontDestroyOnLoad") continue;
				if (seenRoots.Add(root.GetInstanceID())) Walk(root, "DDOL/" + root.name, false, arr, ref count);
			}
			// prefab assets with combat-relevant components (pooled / spawned objects)
			var assetArr = new JArray();
			int assetCount = 0;
			var assetRoots = new HashSet<int>();
			foreach (var comp in Resources.FindObjectsOfTypeAll<Component>())
			{
				if (comp == null) continue;
				if (comp.gameObject.scene.isLoaded || comp.gameObject.scene.name == "DontDestroyOnLoad") continue;
				string tn = comp.GetType().Name;
				bool interesting = false;
				foreach (var n in AssetInterest) if (tn == n) { interesting = true; break; }
				if (!interesting) continue;
				var root = comp.transform.root;
				if (assetRoots.Add(root.GetInstanceID())) Walk(root, "ASSET/" + root.name, true, assetArr, ref assetCount);
			}
			var o = new JObject
			{
				["scene"] = scene,
				["loadedScenes"] = scenes,
				["frame"] = Time.frameCount,
				["count"] = count,
				["assetCount"] = assetCount,
				["objects"] = arr,
				["assets"] = assetArr,
			};
			string path = System.IO.Path.Combine(dir, "hierarchy.json");
			using (var sw = new System.IO.StreamWriter(path, false, new System.Text.UTF8Encoding(false)))
			using (var jw = new JsonTextWriter(sw) { Formatting = Formatting.None })
			{
				o.WriteTo(jw);
			}
			HKOracle.Instance.Log($"[Dump] hierarchy.json: {count} scene objects, {assetCount} asset objects -> {path}");
		}
	}
}
