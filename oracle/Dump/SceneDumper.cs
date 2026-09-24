using System;
using System.Collections.Generic;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using HKOracle.Env;

namespace HKOracle.Dump
{
	// Scene geometry + RNG probe, written by DumpDriver at SceneReady next to the
	// reflection dumps.  scene.json: every Collider2D whose GameObject lives in a
	// loaded scene (Resources.FindObjectsOfTypeAll => includes inactive + pooled
	// objects, excludes prefab assets), with world-space geometry, attached body,
	// and the primitive fields of every non-Unity component on the same object.
	// rng_probe.json: UnityEngine.Random state before/after a fixed sequence of
	// draws so the generator output mapping can be verified offline.
	public static class SceneDumper
	{

		static JToken V(object o) => ReflectionDumper.Val(o, 1);

		static JArray Pts(Transform t, Vector2[] local, Vector2 offset)
		{
			var a = new JArray();
			foreach (var p in local)
			{
				var w = t.TransformPoint(new Vector3(p.x + offset.x, p.y + offset.y, 0f));
				a.Add(new JArray(w.x, w.y));
			}
			return a;
		}

		public static void DumpScene(string dir, string scene)
		{
			var arr = new JArray();
			int skipped = 0;
			foreach (var col in Resources.FindObjectsOfTypeAll<Collider2D>())
			{
				if (col == null || !col.gameObject.scene.isLoaded) { skipped++; continue; }
				var go = col.gameObject;
				var t = col.transform;
				var e = new JObject
				{
					["path"] = ReflectionDumper.Path(t),
					["sceneName"] = go.scene.name,
					["instanceID"] = col.GetInstanceID(),
					["goInstanceID"] = go.GetInstanceID(),
					["type"] = col.GetType().FullName,
					["enabled"] = col.enabled,
					["isActiveAndEnabled"] = col.isActiveAndEnabled,
					["activeInHierarchy"] = go.activeInHierarchy,
					["activeSelf"] = go.activeSelf,
					["isTrigger"] = col.isTrigger,
					["layer"] = go.layer,
					["layerName"] = LayerMask.LayerToName(go.layer),
					["tag"] = go.tag,
					["offset"] = V(col.offset),
					["usedByComposite"] = col.usedByComposite,
					["usedByEffector"] = col.usedByEffector,
					["density"] = col.density,
					["sharedMaterial"] = ReflectionDumper.Material2D(col.sharedMaterial),
					["transform"] = new JObject
					{
						["position"] = V(t.position),
						["lossyScale"] = V(t.lossyScale),
						["eulerZ"] = t.eulerAngles.z,
						["localPosition"] = V(t.localPosition),
						["localScale"] = V(t.localScale),
						["localEulerZ"] = t.localEulerAngles.z,
					},
				};
				try { e["bounds"] = V(col.bounds); } catch (Exception ex) { e["bounds__error"] = ex.Message; }
				var rb = col.attachedRigidbody;
				e["rigidbody"] = rb == null ? (JToken)JValue.CreateNull() : ReflectionDumper.Rigidbody2DState(rb);
				try
				{
					if (col is BoxCollider2D box)
					{
						e["size"] = V(box.size);
						e["edgeRadius"] = box.edgeRadius;
						e["autoTiling"] = box.autoTiling;
						var h = box.size * 0.5f;
						e["world"] = Pts(t, new[] { new Vector2(-h.x, -h.y), new Vector2(h.x, -h.y), new Vector2(h.x, h.y), new Vector2(-h.x, h.y) }, box.offset);
					}
					else if (col is CircleCollider2D cir)
					{
						e["radius"] = cir.radius;
						e["world"] = Pts(t, new[] { Vector2.zero }, cir.offset);
					}
					else if (col is CapsuleCollider2D cap)
					{
						e["size"] = V(cap.size);
						e["direction"] = cap.direction.ToString();
						e["world"] = Pts(t, new[] { Vector2.zero }, cap.offset);
					}
					else if (col is PolygonCollider2D poly)
					{
						e["pathCount"] = poly.pathCount;
						var paths = new JArray(); var world = new JArray();
						for (int i = 0; i < poly.pathCount; i++)
						{
							var p = poly.GetPath(i);
							{ var local = new JArray(); foreach (var q in p) local.Add(new JArray(q.x, q.y)); paths.Add(local); }   // explicit: Val() depth-1 flattens Vector2[] to nothing
							world.Add(Pts(t, p, poly.offset));
						}
						e["paths"] = paths; e["world"] = world;
					}
					else if (col is EdgeCollider2D edge)
					{
						e["edgeRadius"] = edge.edgeRadius;
						{ var local = new JArray(); foreach (var q in edge.points) local.Add(new JArray(q.x, q.y)); e["points"] = local; }
						e["world"] = Pts(t, edge.points, edge.offset);
					}
					else if (col is CompositeCollider2D comp)
					{
						e["geometryType"] = comp.geometryType.ToString();
						e["generationType"] = comp.generationType.ToString();
						e["edgeRadius"] = comp.edgeRadius;
						e["vertexDistance"] = comp.vertexDistance;
						e["offsetDistance"] = comp.offsetDistance;
						e["pathCount"] = comp.pathCount;
						var world = new JArray();
						for (int i = 0; i < comp.pathCount; i++)
						{
							var p = new Vector2[comp.GetPathPointCount(i)];
							comp.GetPath(i, p);
							// composite paths are in the composite body's local space; offset is folded in
							world.Add(Pts(t, p, Vector2.zero));
						}
						e["world"] = world;
					}
				}
				catch (Exception ex) { e["shape__error"] = ex.Message; }

				var comps = new JArray();
				foreach (var c in go.GetComponents<Component>())
				{
					if (c == null) continue;
					var ty = c.GetType();
					var cj = new JObject { ["type"] = ty.FullName };
					if (c is Behaviour b) cj["enabled"] = b.enabled;
					if (!ty.FullName.StartsWith("UnityEngine.") && !ty.FullName.StartsWith("tk2d") && !(c is Collider2D))
					{
						try { cj["fields"] = ReflectionDumper.FieldDump(c, typeof(MonoBehaviour)); }
						catch (Exception ex) { cj["fields__error"] = ex.Message; }
					}
					comps.Add(cj);
				}
				e["components"] = comps;
				arr.Add(e);
			}
			var root = new JObject
			{
				["scene"] = scene,
				["count"] = arr.Count,
				["skippedNotLoaded"] = skipped,
				["frame"] = Time.frameCount,
				["colliders"] = arr,
			};
			ReflectionDumper.WriteJson(dir, "scene.json", root, Formatting.Indented);
		}

		static JObject St() => JObject.Parse(JsonUtility.ToJson(UnityEngine.Random.state));

		public static void DumpRngProbe(string dir)
		{
			var saved = UnityEngine.Random.state;
			var log = new JArray();
			void Rec(string op, Func<object> f)
			{
				var before = St();
				object r = f();
				log.Add(new JObject { ["op"] = op, ["before"] = before, ["result"] = r == null ? JValue.CreateNull() : V(r), ["after"] = St() });
			}
			Rec("InitState(12345)", () => { UnityEngine.Random.InitState(12345); return null; });
			Rec("Range(1f,1f)", () => UnityEngine.Random.Range(1f, 1f));
			Rec("Range(3,3)", () => UnityEngine.Random.Range(3, 3));
			Rec("Range(0,5)", () => UnityEngine.Random.Range(0, 5));
			Rec("Range(0f,1f)", () => UnityEngine.Random.Range(0f, 1f));
			Rec("value", () => UnityEngine.Random.value);
			Rec("Range(-2.5f,7.5f)", () => UnityEngine.Random.Range(-2.5f, 7.5f));
			Rec("Range(0,1000000)", () => UnityEngine.Random.Range(0, 1000000));
			Rec("Range(7,2)", () => UnityEngine.Random.Range(7, 2));
			Rec("Range(-3,3)", () => UnityEngine.Random.Range(-3, 3));
			Rec("Range(2f,1f)", () => UnityEngine.Random.Range(2f, 1f));
			Rec("insideUnitCircle", () => UnityEngine.Random.insideUnitCircle);
			Rec("Range(0,2147483647)", () => UnityEngine.Random.Range(0, int.MaxValue));
			Rec("Range(0,1)", () => UnityEngine.Random.Range(0, 1));
			Rec("Range(0f,0f)", () => UnityEngine.Random.Range(0f, 0f));
			Rec("value", () => UnityEngine.Random.value);
			Rec("value", () => UnityEngine.Random.value);
			Rec("value", () => UnityEngine.Random.value);
			Rec("InitState(0)", () => { UnityEngine.Random.InitState(0); return null; });
			Rec("InitState(-1)", () => { UnityEngine.Random.InitState(-1); return null; });
			Rec("InitState(2026)", () => { UnityEngine.Random.InitState(2026); return null; });
			Rec("value", () => UnityEngine.Random.value);
			// Pin the float Range operation order with odd ranges (200 float + 60 int draws).
			for (int i = 0; i < 200; i++)
			{
				float lo = (i * 0.731f) - 40f;
				float hi = lo + ((i % 7) + 1) * 1.913f * ((i % 3 == 0) ? -1f : 1f);
				Rec("Range(" + lo.ToString("R") + "f," + hi.ToString("R") + "f)", () => UnityEngine.Random.Range(lo, hi));
			}
			for (int i = 0; i < 60; i++)
			{
				int lo = (i * 37) - 500;
				int hi = lo + ((i % 11) + 1) * 13 * ((i % 4 == 0) ? -1 : 1);
				Rec("Range(" + lo + "," + hi + ")", () => UnityEngine.Random.Range(lo, hi));
			}
			UnityEngine.Random.state = saved;
			ReflectionDumper.WriteJson(dir, "rng_probe.json", new JObject { ["draws"] = log }, Formatting.Indented);
		}
	}
}
