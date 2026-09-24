using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using HKOracle.Env;

namespace HKOracle.Dump
{
	// Runtime ground truth for everything the simulator has to reproduce that is
	// NOT in the decompiled source: inspector-serialized field values, Unity
	// physics settings, collider geometry, tk2d animation tables.
	// Nothing here interprets or normalizes: values are written as read.
	public static class ReflectionDumper
	{
		private const BindingFlags kInstAll =
			BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

		// Depth budget for the generic value serializer. Guards reference cycles
		// (Fsm <-> FsmState, Transform.parent chains reached through fields) and
		// pathological nesting. Exceeded => explicit __unserialized, never a drop.
		private const int kMaxRecursion = 12;

		[ThreadStatic] private static int _rec;

		public static int UnserializedCount;
		public static int ErrorCount;

		private static void Log(string m) => HKOracle.Instance.Log("[Dump] " + m);

		// ---------------------------------------------------------------- values

		public static JToken Val(object v, int depth)
		{
			if (v == null) return JValue.CreateNull();
			if (_rec > kMaxRecursion) return Unser(v.GetType(), "recursion-limit");
			_rec++;
			try { return ValInner(v, depth); }
			catch (Exception e) { ErrorCount++; return new JObject { ["__error"] = HKOracle.DescribeException(e) }; }
			finally { _rec--; }
		}

		private static JToken ValInner(object v, int depth)
		{
			var t = v.GetType();

			if (v is string s) return new JValue(s);
			if (t.IsEnum)
			{
				var eo = new JObject { ["__enum"] = t.FullName, ["name"] = v.ToString() };
				try { eo["value"] = new JValue(Convert.ToInt64(v)); } catch { eo["value"] = JValue.CreateNull(); }
				return eo;
			}
			if (t.IsPrimitive || v is decimal) return new JValue(v);

			if (v is Vector2 v2) return new JObject { ["x"] = v2.x, ["y"] = v2.y };
			if (v is Vector3 v3) return new JObject { ["x"] = v3.x, ["y"] = v3.y, ["z"] = v3.z };
			if (v is Vector4 v4) return new JObject { ["x"] = v4.x, ["y"] = v4.y, ["z"] = v4.z, ["w"] = v4.w };
			if (v is Vector2Int v2i) return new JObject { ["x"] = v2i.x, ["y"] = v2i.y };
			if (v is Vector3Int v3i) return new JObject { ["x"] = v3i.x, ["y"] = v3i.y, ["z"] = v3i.z };
			if (v is Quaternion q) return new JObject { ["x"] = q.x, ["y"] = q.y, ["z"] = q.z, ["w"] = q.w };
			if (v is Color c) return new JObject { ["r"] = c.r, ["g"] = c.g, ["b"] = c.b, ["a"] = c.a };
			if (v is Color32 c32) return new JObject { ["r"] = c32.r, ["g"] = c32.g, ["b"] = c32.b, ["a"] = c32.a };
			if (v is Rect r) return new JObject { ["x"] = r.x, ["y"] = r.y, ["width"] = r.width, ["height"] = r.height };
			if (v is Bounds b) return new JObject
			{
				["center"] = Val(b.center, depth + 1),
				["size"] = Val(b.size, depth + 1),
			};
			if (v is LayerMask lm) return new JObject { ["__layerMask"] = lm.value };
			if (v is Type ty) return new JObject { ["__systemType"] = ty.FullName ?? ty.Name };

			if (v is UnityEngine.Object uo) return UnityRef(uo);

			if (v is Delegate del)
			{
				UnserializedCount++;
				var o = new JObject { ["__type"] = t.FullName, ["__unserialized"] = true };
				var arr = new JArray();
				try
				{
					foreach (var d in del.GetInvocationList())
						arr.Add((d.Method?.DeclaringType?.FullName ?? "?") + "." + (d.Method?.Name ?? "?"));
				}
				catch { }
				o["__invocationList"] = arr;
				return o;
			}

			if (v is IDictionary dict)
			{
				var arr = new JArray();
				foreach (DictionaryEntry e in dict)
					arr.Add(new JObject { ["key"] = Val(e.Key, depth), ["value"] = Val(e.Value, depth) });
				return new JObject { ["__dict"] = t.FullName, ["count"] = arr.Count, ["entries"] = arr };
			}

			if (v is IEnumerable en)
			{
				var arr = new JArray();
				int n = 0;
				foreach (var e in en)
				{
					if (n++ >= 16384) { arr.Add(new JObject { ["__truncated_after"] = 16384 }); break; }
					arr.Add(Val(e, depth));
				}
				return arr;
			}

			if (depth < 1) return OneLevel(v, t);
			return Unser(t, "depth");
		}

		private static JObject Unser(Type t, string why)
		{
			UnserializedCount++;
			return new JObject { ["__type"] = t.FullName, ["__unserialized"] = true, ["__reason"] = why };
		}

		// "one level deep (their primitive fields)": every instance field of a
		// plain managed object, with nested plain objects degrading to
		// __unserialized (depth budget of 1 is spent by the dive itself).
		private static JObject OneLevel(object v, Type t)
		{
			var o = new JObject { ["__type"] = t.FullName };
			foreach (var f in Fields(t, typeof(object)))
			{
				try { o[f.Name] = Val(f.GetValue(v), 1); }
				catch (Exception e) { ErrorCount++; o[f.Name] = new JObject { ["__error"] = e.GetType().Name + ": " + e.Message }; }
			}
			return o;
		}

		// PhysicsMaterial2D values (friction, bounciness), not just the reference: the contact solver mixes them.
		public static JToken Material2D(PhysicsMaterial2D m)
		{
			if (m == null) return JValue.CreateNull();
			var o = (JObject)UnityRef(m);
			o["friction"] = m.friction;
			o["bounciness"] = m.bounciness;
			return o;
		}

		// Every Rigidbody2D property the solver reads, with the object's instance id.
		public static JObject Rigidbody2DState(Rigidbody2D rb)
		{
			var o = new JObject
			{
				["instanceID"] = rb.GetInstanceID(),
				["path"] = Path(rb.transform),
				["bodyType"] = rb.bodyType.ToString(),
				["isKinematic"] = rb.isKinematic,
				["simulated"] = rb.simulated,
				["useFullKinematicContacts"] = rb.useFullKinematicContacts,
				["useAutoMass"] = rb.useAutoMass,
				["mass"] = rb.mass,
				["inertia"] = rb.inertia,
				["centerOfMass"] = Val(rb.centerOfMass, 1),
				["drag"] = rb.drag,
				["angularDrag"] = rb.angularDrag,
				["gravityScale"] = rb.gravityScale,
				["interpolation"] = rb.interpolation.ToString(),
				["collisionDetectionMode"] = rb.collisionDetectionMode.ToString(),
				["sleepMode"] = rb.sleepMode.ToString(),
				["constraints"] = rb.constraints.ToString(),
				["freezeRotation"] = rb.freezeRotation,
				["position"] = Val(rb.position, 1),
				["rotation"] = rb.rotation,
				["velocity"] = Val(rb.velocity, 1),
				["angularVelocity"] = rb.angularVelocity,
				["isAwake"] = rb.IsAwake(),
				["attachedColliderCount"] = rb.attachedColliderCount,
				["sharedMaterial"] = Material2D(rb.sharedMaterial),
			};
			return o;
		}

		public static JToken UnityRef(UnityEngine.Object uo)
		{
			// A destroyed UnityEngine.Object is a live managed reference with a
			// dead native peer; `== null` is the only way to see that.
			bool destroyed = uo == null;
			var o = new JObject
			{
				["type"] = uo.GetType().FullName,
				["name"] = destroyed ? null : SafeName(uo),
				["instanceID"] = uo.GetInstanceID(),
			};
			if (destroyed) { o["__destroyed"] = true; return o; }
			var go = uo as GameObject ?? (uo as Component)?.gameObject;
			if (go != null) o["path"] = Path(go.transform);
			return o;
		}

		private static string SafeName(UnityEngine.Object uo)
		{
			try { return uo.name; } catch { return null; }
		}

		public static List<FieldInfo> Fields(Type t, Type stopBase)
		{
			var list = new List<FieldInfo>();
			for (var cur = t; cur != null && cur != stopBase && cur != typeof(object); cur = cur.BaseType)
				list.AddRange(cur.GetFields(kInstAll));
			return list;
		}

		public static JArray FieldDump(object obj, Type stopBase)
		{
			var arr = new JArray();
			if (obj == null) return arr;
			foreach (var f in Fields(obj.GetType(), stopBase))
			{
				var e = new JObject
				{
					["name"] = f.Name,
					["type"] = f.FieldType.FullName ?? f.FieldType.Name,
					["declaringType"] = f.DeclaringType?.FullName,
				};
				try { e["value"] = Val(f.GetValue(obj), 0); }
				catch (Exception ex) { ErrorCount++; e["__error"] = HKOracle.DescribeException(ex); }
				arr.Add(e);
			}
			return arr;
		}

		public static string Path(Transform t)
		{
			if (t == null) return null;
			var sb = new System.Text.StringBuilder(t.name);
			for (var p = t.parent; p != null; p = p.parent) sb.Insert(0, p.name + "/");
			return sb.ToString();
		}

		// ---------------------------------------------------------------- files

		public static void WriteJson(string dir, string file, JToken tok, Formatting fmt)
		{
			System.IO.Directory.CreateDirectory(dir);
			string path = System.IO.Path.Combine(dir, file);
			using (var sw = new System.IO.StreamWriter(path, false, new System.Text.UTF8Encoding(false)))
			using (var jw = new JsonTextWriter(sw) { Formatting = fmt })
			{
				tok.WriteTo(jw);
				jw.Flush();
				sw.Flush();
			}
			var fi = new System.IO.FileInfo(path);
			Log($"wrote {path} ({fi.Length} bytes)");
		}

		public static void DumpAll(string sceneDir, string scene, string levelRequested)
		{
			Run(sceneDir, "hero.json", scene, Hero);
			Run(sceneDir, "playerdata.json", scene, PlayerDataDump);
			Run(sceneDir, "physics.json", scene, Physics);
			Run(sceneDir, "bosses.json", scene, Bosses);
			Run(sceneDir, "meta.json", scene, () => Meta(scene, levelRequested));
		}

		private static void Run(string dir, string file, string scene, Func<JToken> f)
		{
			try
			{
				int u0 = UnserializedCount, e0 = ErrorCount;
				var tok = f();
				WriteJson(dir, file, tok, Formatting.Indented);
				Log($"{file}: unserialized+={UnserializedCount - u0} errors+={ErrorCount - e0}");
			}
			catch (Exception e)
			{
				ErrorCount++;
				Log($"{file} FAILED: {HKOracle.DescribeException(e)}");
				try { WriteJson(dir, file, new JObject { ["__error"] = HKOracle.DescribeException(e) }, Formatting.Indented); }
				catch { }
			}
		}

		// ---------------------------------------------------------------- hero

		private static JToken Hero()
		{
			var hc = HeroController.instance;
			var o = new JObject();
			if (hc == null) { o["__error"] = "HeroController.instance == null"; return o; }
			var go = hc.gameObject;

			o["gameObject"] = new JObject
			{
				["path"] = Path(go.transform),
				["name"] = go.name,
				["layer"] = go.layer,
				["layerName"] = LayerMask.LayerToName(go.layer),
				["tag"] = go.tag,
				["activeSelf"] = go.activeSelf,
				["activeInHierarchy"] = go.activeInHierarchy,
				["scene"] = go.scene.name,
				["instanceID"] = go.GetInstanceID(),
			};
			o["transform"] = new JObject
			{
				["position"] = Val(go.transform.position, 1),
				["localPosition"] = Val(go.transform.localPosition, 1),
				["localScale"] = Val(go.transform.localScale, 1),
				["lossyScale"] = Val(go.transform.lossyScale, 1),
				["rotation"] = Val(go.transform.rotation, 1),
			};

			// HeroController: declared on it and its bases up to (excluding)
			// MonoBehaviour. HeroController : MonoBehaviour directly.
			// analysis/decomp/Assembly-CSharp/HeroController.cs:9
			o["heroController"] = FieldDump(hc, typeof(MonoBehaviour));

			// analysis/decomp/Assembly-CSharp/HeroControllerStates.cs
			HeroControllerStates cs = null;
			try { cs = hc.cState; } catch (Exception e) { o["cState_error"] = HKOracle.DescribeException(e); }
			o["cState"] = FieldDump(cs, typeof(object));

			o["components"] = ComponentTree(go);
			return o;
		}

		private static JArray ComponentTree(GameObject root)
		{
			var arr = new JArray();
			foreach (var t in root.GetComponentsInChildren<Transform>(true))
			{
				var entry = new JObject
				{
					["path"] = Path(t),
					["activeSelf"] = t.gameObject.activeSelf,
					["activeInHierarchy"] = t.gameObject.activeInHierarchy,
					["layer"] = t.gameObject.layer,
					["layerName"] = LayerMask.LayerToName(t.gameObject.layer),
					["tag"] = t.gameObject.tag,
				};
				var comps = new JArray();
				foreach (var comp in t.GetComponents<Component>())
					comps.Add(comp == null ? "<missing script>" : comp.GetType().FullName);
				entry["components"] = comps;
				arr.Add(entry);
			}
			return arr;
		}

		// ---------------------------------------------------------- playerdata

		private static JToken PlayerDataDump()
		{
			var pd = PlayerData.instance;
			var o = new JObject();
			if (pd == null) { o["__error"] = "PlayerData.instance == null"; return o; }
			o["type"] = pd.GetType().FullName;
			o["fields"] = FieldDump(pd, typeof(object));
			return o;
		}

		// ------------------------------------------------------------- physics

		private static JToken Physics()
		{
			var o = new JObject();
			var hc = HeroController.instance;

			// rb2d is a private field on HeroController; oracle/Game/StateExtractor.cs
			// reads it with Modding.ReflectionHelper.GetField.
			Rigidbody2D rb = null;
			if (hc != null)
			{
				try { rb = Modding.ReflectionHelper.GetField<HeroController, Rigidbody2D>(hc, "rb2d"); }
				catch (Exception e) { o["rb2d_error"] = HKOracle.DescribeException(e); }
			}
			o["rb2d"] = rb == null ? (JToken)JValue.CreateNull() : PropDump(rb, typeof(Rigidbody2D));

			if (hc != null) o["heroColliders"] = Colliders(hc.gameObject);

			o["Physics2D"] = StaticPropDump(typeof(Physics2D));
			o["layerNames"] = LayerNames();
			o["layerCollisionMatrix"] = LayerMatrix();

			o["Time"] = new JObject
			{
				["fixedDeltaTime"] = Time.fixedDeltaTime,
				["maximumDeltaTime"] = Time.maximumDeltaTime,
				["captureDeltaTime"] = Time.captureDeltaTime,
				["timeScale"] = Time.timeScale,
				["maximumParticleDeltaTime"] = Time.maximumParticleDeltaTime,
				["frameCount"] = Time.frameCount,
				["time"] = Time.time,
				["unscaledTime"] = Time.unscaledTime,
				["fixedTime"] = Time.fixedTime,
				["realtimeSinceStartup"] = Time.realtimeSinceStartup,
			};
			o["Application"] = new JObject
			{
				["targetFrameRate"] = Application.targetFrameRate,
				["isBatchMode"] = Application.isBatchMode,
			};
			o["QualitySettings"] = new JObject
			{
				["vSyncCount"] = QualitySettings.vSyncCount,
			};

			if (hc != null)
			{
				var anim = hc.GetComponent<tk2dSpriteAnimator>();
				o["heroAnimator"] = anim == null ? (JToken)JValue.CreateNull() : Animator(anim);
				var spr = hc.GetComponent<tk2dBaseSprite>();
				o["heroSprite"] = spr == null ? (JToken)JValue.CreateNull() : Sprite(spr);
			}
			return o;
		}

		private static JObject PropDump(object obj, Type declaringSearch)
		{
			var o = new JObject();
			foreach (var p in declaringSearch.GetProperties(BindingFlags.Public | BindingFlags.Instance))
			{
				if (!p.CanRead || p.GetIndexParameters().Length > 0) continue;
				try { o[p.Name] = Val(p.GetValue(obj, null), 0); }
				catch (Exception e) { ErrorCount++; o[p.Name] = new JObject { ["__error"] = e.GetType().Name + ": " + e.Message }; }
			}
			return o;
		}

		private static JObject StaticPropDump(Type t)
		{
			var o = new JObject();
			foreach (var p in t.GetProperties(BindingFlags.Public | BindingFlags.Static))
			{
				if (!p.CanRead || p.GetIndexParameters().Length > 0) continue;
				try { o[p.Name] = Val(p.GetValue(null, null), 0); }
				catch (Exception e) { ErrorCount++; o[p.Name] = new JObject { ["__error"] = e.GetType().Name + ": " + e.Message }; }
			}
			return o;
		}

		private static JArray LayerNames()
		{
			var arr = new JArray();
			for (int i = 0; i < 32; i++) arr.Add(LayerMask.LayerToName(i));
			return arr;
		}

		private static JObject LayerMatrix()
		{
			var o = new JObject();
			var m = new JArray();
			for (int i = 0; i < 32; i++)
			{
				var row = new JArray();
				for (int j = 0; j < 32; j++) row.Add(Physics2D.GetIgnoreLayerCollision(i, j));
				m.Add(row);
			}
			o["ignoreLayerCollision"] = m;   // [i][j] == true  =>  layers i and j do NOT collide
			return o;
		}

		public static JArray Colliders(GameObject root)
		{
			var arr = new JArray();
			foreach (var col in root.GetComponentsInChildren<Collider2D>(true))
			{
				var e = new JObject
				{
					["path"] = Path(col.transform),
					["type"] = col.GetType().FullName,
					["enabled"] = col.enabled,
					["isActiveAndEnabled"] = col.isActiveAndEnabled,
					["isTrigger"] = col.isTrigger,
					["offset"] = Val(col.offset, 1),
					["layer"] = col.gameObject.layer,
					["layerName"] = LayerMask.LayerToName(col.gameObject.layer),
					["usedByComposite"] = col.usedByComposite,
					["usedByEffector"] = col.usedByEffector,
					["density"] = col.density,
					["sharedMaterial"] = col.sharedMaterial == null ? (JToken)JValue.CreateNull() : UnityRef(col.sharedMaterial),
				};
				try { e["bounds"] = Val(col.bounds, 1); } catch (Exception ex) { e["bounds__error"] = ex.Message; }
				if (col is BoxCollider2D box)
				{
					e["size"] = Val(box.size, 1);
					e["edgeRadius"] = box.edgeRadius;
					e["autoTiling"] = box.autoTiling;
				}
				else if (col is CircleCollider2D cir) e["radius"] = cir.radius;
				else if (col is CapsuleCollider2D cap)
				{
					e["size"] = Val(cap.size, 1);
					e["direction"] = Val(cap.direction, 1);
				}
				else if (col is PolygonCollider2D poly)
				{
					e["pathCount"] = poly.pathCount;
					var paths = new JArray();
					for (int i = 0; i < poly.pathCount; i++) paths.Add(Val(poly.GetPath(i), 1));
					e["paths"] = paths;
				}
				else if (col is EdgeCollider2D edge)
				{
					e["edgeRadius"] = edge.edgeRadius;
					e["points"] = Val(edge.points, 1);
				}
				else if (col is CompositeCollider2D comp)
				{
					e["geometryType"] = Val(comp.geometryType, 1);
					e["generationType"] = Val(comp.generationType, 1);
					e["edgeRadius"] = comp.edgeRadius;
					e["vertexDistance"] = comp.vertexDistance;
					e["pathCount"] = comp.pathCount;
				}
				arr.Add(e);
			}
			return arr;
		}

		// ---------------------------------------------------------------- tk2d

		// One library asset is shared by every instance of an enemy prefab; emit
		// it once per file and reference it by instanceID afterwards.
		private static HashSet<int> _libsEmitted;

		public static JToken Animator(tk2dSpriteAnimator anim)
		{
			var o = new JObject
			{
				["type"] = anim.GetType().FullName,
				["path"] = Path(anim.transform),
				["enabled"] = anim.enabled,
				["playAutomatically"] = anim.playAutomatically,
				["paused"] = anim.Paused,
				["playing"] = anim.Playing,
				["defaultClipId"] = anim.DefaultClipId,
			};
			try { o["currentClip"] = anim.CurrentClip == null ? null : anim.CurrentClip.name; } catch (Exception e) { o["currentClip__error"] = e.Message; }
			try { o["currentFrame"] = anim.CurrentFrame; } catch (Exception e) { o["currentFrame__error"] = e.Message; }
			try { o["clipTimeSeconds"] = anim.ClipTimeSeconds; } catch (Exception e) { o["clipTimeSeconds__error"] = e.Message; }
			try { o["clipFps"] = anim.ClipFps; } catch (Exception e) { o["clipFps__error"] = e.Message; }

			var lib = anim.Library;
			if (lib == null) { o["library"] = JValue.CreateNull(); return o; }
			int id = lib.GetInstanceID();
			if (_libsEmitted != null && !_libsEmitted.Add(id))
			{
				o["library"] = new JObject { ["__ref_instanceID"] = id, ["name"] = lib.name };
				return o;
			}
			o["library"] = Library(lib);
			return o;
		}

		private static JObject Library(tk2dSpriteAnimation lib)
		{
			// analysis/decomp/Assembly-CSharp/tk2dSpriteAnimationClip.cs:18-26
			// analysis/decomp/Assembly-CSharp/tk2dSpriteAnimationFrame.cs:6-16
			var o = new JObject { ["name"] = lib.name, ["instanceID"] = lib.GetInstanceID() };
			var clips = new JArray();
			var arr = lib.clips;
			if (arr != null)
			{
				for (int i = 0; i < arr.Length; i++)
				{
					var cl = arr[i];
					if (cl == null) { clips.Add(JValue.CreateNull()); continue; }
					var co = new JObject
					{
						["id"] = i,
						["name"] = cl.name,
						["fps"] = cl.fps,
						["wrapMode"] = Val(cl.wrapMode, 1),
						["loopStart"] = cl.loopStart,
						["frameCount"] = cl.frames == null ? 0 : cl.frames.Length,
					};
					var fr = new JArray();
					if (cl.frames != null)
					{
						for (int j = 0; j < cl.frames.Length; j++)
						{
							var f = cl.frames[j];
							if (f == null) { fr.Add(JValue.CreateNull()); continue; }
							var fo = new JObject
							{
								["index"] = j,
								["spriteId"] = f.spriteId,
								["triggerEvent"] = f.triggerEvent,
								["eventInfo"] = f.eventInfo,
								["eventInt"] = f.eventInt,
								["eventFloat"] = f.eventFloat,
								["spriteCollection"] = f.spriteCollection == null ? null : f.spriteCollection.name,
								["spriteName"] = SpriteName(f.spriteCollection, f.spriteId),
							};
							fr.Add(fo);
						}
					}
					co["frames"] = fr;
					clips.Add(co);
				}
			}
			o["clips"] = clips;
			return o;
		}

		private static string SpriteName(tk2dSpriteCollectionData coll, int spriteId)
		{
			if (coll == null) return null;
			try
			{
				var inst = coll.inst;
				if (inst == null || inst.spriteDefinitions == null) return null;
				if (spriteId < 0 || spriteId >= inst.spriteDefinitions.Length) return null;
				return inst.spriteDefinitions[spriteId]?.name;
			}
			catch { return null; }
		}

		private static JToken Sprite(tk2dBaseSprite spr)
		{
			var o = new JObject
			{
				["type"] = spr.GetType().FullName,
				["spriteId"] = spr.spriteId,
				["scale"] = Val(spr.scale, 1),
				["collection"] = spr.Collection == null ? null : spr.Collection.name,
				["boxCollider2D"] = spr.boxCollider2D == null ? (JToken)JValue.CreateNull() : UnityRef(spr.boxCollider2D),
			};
			try { o["bounds"] = Val(spr.GetBounds(), 1); } catch (Exception e) { o["bounds__error"] = e.Message; }
			try { o["untrimmedBounds"] = Val(spr.GetUntrimmedBounds(), 1); } catch (Exception e) { o["untrimmedBounds__error"] = e.Message; }
			try
			{
				var def = spr.GetCurrentSpriteDef();
				if (def != null)
					o["currentSpriteDef"] = new JObject
					{
						["name"] = def.name,
						["colliderType"] = Val(def.colliderType, 1),
						["physicsEngine"] = Val(def.physicsEngine, 1),
						["boundsData"] = Val(def.boundsData, 1),
						["untrimmedBoundsData"] = Val(def.untrimmedBoundsData, 1),
						["colliderVertexCount"] = def.colliderVertices == null ? 0 : def.colliderVertices.Length,
						["colliderConvex"] = def.colliderConvex,
						["texelSize"] = Val(def.texelSize, 1),
					};
			}
			catch (Exception e) { o["currentSpriteDef__error"] = HKOracle.DescribeException(e); }
			return o;
		}

		// -------------------------------------------------------------- bosses

		private static JToken Bosses()
		{
			_libsEmitted = new HashSet<int>();
			var arr = new JArray();
			var all = Resources.FindObjectsOfTypeAll<HealthManager>();
			int kept = 0;
			// This file lists EVERY HealthManager in the loaded scenes, which is NOT the set the mod
			// treats as "the boss".  TrainingEnv.InitBossRefs binds the union of
			// BossSceneController.bosses and BossHealthLookup.Keys, and the size of that set is the
			// denominator `n` in the damage_landed reward, so each HealthManager carries a flag saying
			// whether it is in that set (per row, so a consumer cannot pair them up wrongly).
			var bossSet = new HashSet<HealthManager>();
			int bossesLen = -1, lookupLen = -1;
			try
			{
				var bsc = BossSceneController.Instance;
				if (bsc != null)
				{
					if (bsc.bosses != null)
					{
						bossesLen = bsc.bosses.Length;
						foreach (var b in bsc.bosses) if (b != null) bossSet.Add(b);
					}
					if (bsc.BossHealthLookup != null)
					{
						lookupLen = bsc.BossHealthLookup.Count;
						foreach (var k in bsc.BossHealthLookup.Keys) if (k != null) bossSet.Add(k);
					}
				}
			}
			catch (Exception e) { HKOracle.Instance.Log("[Dump] bosses: BossSceneController read failed: " + HKOracle.DescribeException(e)); }
			foreach (var hm in all)
			{
				if (hm == null) continue;
				GameObject go;
				try { go = hm.gameObject; } catch { continue; }
				if (go == null) continue;
				// Prefab assets live in an unloaded pseudo-scene; scene instances
				// (including pooled/inactive ones) have isLoaded == true.
				if (!go.scene.isLoaded) continue;
				kept++;
				var o = new JObject
				{
					["path"] = Path(go.transform),
					["name"] = go.name,
					["scene"] = go.scene.name,
					["instanceID"] = hm.GetInstanceID(),
					["activeSelf"] = go.activeSelf,
					["activeInHierarchy"] = go.activeInHierarchy,
					["enabled"] = hm.enabled,
					["layer"] = go.layer,
					["layerName"] = LayerMask.LayerToName(go.layer),
					["tag"] = go.tag,
				};
				// True when this HealthManager is in the set TrainingEnv.InitBossRefs would bind.
				// NOTE it is captured at SceneReady; BossSceneController.bosses can populate lazily
				// (TrainingEnv.Reset retries for it), so a consumer must treat bossesLen == 0 as
				// "the array was not populated yet at this instant", not as "this scene has no bosses".
				o["inBossesArray"] = bossSet.Contains(hm);
				try { o["hp"] = hm.hp; } catch (Exception e) { o["hp__error"] = e.Message; }
				try { o["isDead"] = hm.isDead; } catch { }
				try { o["isInvincible"] = hm.IsInvincible; } catch { }
				o["transform"] = new JObject
				{
					["position"] = Val(go.transform.position, 1),
					["localScale"] = Val(go.transform.localScale, 1),
					["rotation"] = Val(go.transform.rotation, 1),
				};
				// analysis/decomp/Assembly-CSharp/HealthManager.cs:9 (: MonoBehaviour)
				o["fields"] = FieldDump(hm, typeof(MonoBehaviour));
				var rb = go.GetComponent<Rigidbody2D>();
				o["rb2d"] = rb == null ? (JToken)JValue.CreateNull() : PropDump(rb, typeof(Rigidbody2D));
				o["colliders"] = Colliders(go);
				var anim = go.GetComponent<tk2dSpriteAnimator>();
				o["animator"] = anim == null ? (JToken)JValue.CreateNull() : Animator(anim);
				o["components"] = ComponentTree(go);
				arr.Add(o);
			}
			Log($"bosses.json: {kept} HealthManagers in loaded scenes (of {all.Length} found); "
				+ $"BossSceneController.bosses={bossesLen} BossHealthLookup={lookupLen} boundSet={bossSet.Count}");
			return new JObject
			{
				["count"] = kept,
				["bossesLen"] = bossesLen,
				["bossHealthLookupLen"] = lookupLen,
				["boundSetCount"] = bossSet.Count,
				["healthManagers"] = arr,
			};
		}

		// ---------------------------------------------------------------- meta

		private static JToken Meta(string scene, string levelRequested)
		{
			var envs = new JObject();
			foreach (var k in new[] { "HK_ORACLE_DUMPS", "HK_ORACLE_LEVEL", "HK_ORACLE_SCRIPT",
				"HK_ORACLE_TRACE", "HK_ORACLE_SEED", "HK_ORACLE_DIR", "FK_SERVER_URL" })
				envs[k] = System.Environment.GetEnvironmentVariable(k);

			var scenes = new JArray();
			for (int i = 0; i < UnityEngine.SceneManagement.SceneManager.sceneCount; i++)
			{
				var sc = UnityEngine.SceneManagement.SceneManager.GetSceneAt(i);
				scenes.Add(new JObject { ["name"] = sc.name, ["isLoaded"] = sc.isLoaded, ["buildIndex"] = sc.buildIndex });
			}

			return new JObject
			{
				["scene"] = scene,
				["levelRequested"] = levelRequested,
				["loadedScenes"] = scenes,
				["unityVersion"] = Application.unityVersion,
				["productName"] = Application.productName,
				["version"] = Application.version,
				["platform"] = Val(Application.platform, 1),
				["timestampUtc"] = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"),
				["modCommit"] = ModInfo.Commit,
				["oracleAssembly"] = typeof(ReflectionDumper).Assembly.Location,
				["processName"] = System.Diagnostics.Process.GetCurrentProcess().ProcessName,
				["commandLine"] = System.Environment.CommandLine,
				["timeScaleAtDump"] = Time.timeScale,
				["frameCountAtDump"] = Time.frameCount,
				["unserializedCount"] = UnserializedCount,
				["errorCount"] = ErrorCount,
			};
		}
	}
}
