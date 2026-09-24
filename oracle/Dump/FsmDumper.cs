using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using HutongGames.PlayMaker;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using HKOracle.Env;

namespace HKOracle.Dump
{
	// Structural dump of every PlayMakerFSM in the loaded scenes: states,
	// transitions, variables, and every action with every public field.
	// This is the input to the simulator's FSM interpreter, so nothing is filtered:
	// inactive objects (pooled projectiles) are included, and any field the
	// serializer does not understand is emitted as an explicit
	// {"__type":..., "__unserialized":true} rather than dropped.
	//
	// Member names come from the decomp, not from memory:
	//   analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:458-542
	//     (Name, StartState, States, Events, GlobalTransitions, Variables)
	//   analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmState.cs:96-191
	//     (Name, IsSequence, Actions, Transitions)
	//   analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmTransition.cs:42-125
	//     (FsmEvent, ToState, LinkStyle, EventName)
	//   analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmVariables.cs:92-260
	//     (the fifteen typed variable arrays)
	//   analysis/decomp/PlayMaker/HutongGames.PlayMaker/NamedVariable.cs:39-96
	//     (Name, UseVariable, RawValue)
	public static class FsmDumper
	{
		public static int ActionCount;
		public static int ActionErrorCount;
		public static int UnserializedCount;
		public static int FsmCount;
		public static int AssetFsmCount;
		public static int AssetActionCount;
		[ThreadStatic] private static bool _asset;   // the FSM being written is a prefab asset's: counted apart
		public static int InitDataCount;
		public static readonly HashSet<string> ActionTypes = new HashSet<string>();

		private const int kMaxRecursion = 10;
		[ThreadStatic] private static int _rec;

		private static void Log(string m) => HKOracle.Instance.Log("[Dump] " + m);

		public static void Dump(string sceneDir, string scene, string levelRequested)
		{
			ActionCount = 0; ActionErrorCount = 0; UnserializedCount = 0; FsmCount = 0; AssetFsmCount = 0; AssetActionCount = 0; InitDataCount = 0;
			ActionTypes.Clear();

			System.IO.Directory.CreateDirectory(sceneDir);
			string path = System.IO.Path.Combine(sceneDir, "fsm.json");

			var all = Resources.FindObjectsOfTypeAll<PlayMakerFSM>();
			Log($"fsm: {all.Length} PlayMakerFSM objects found (pre scene filter)");

			using (var sw = new System.IO.StreamWriter(path, false, new System.Text.UTF8Encoding(false)))
			// Compact: indented output is too large for a single file.
			using (var jw = new JsonTextWriter(sw) { Formatting = Formatting.None })
			{
				jw.WriteStartObject();
				jw.WritePropertyName("scene"); jw.WriteValue(scene);
				jw.WritePropertyName("levelRequested"); jw.WriteValue(levelRequested);
				jw.WritePropertyName("timestampUtc"); jw.WriteValue(DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ"));
				jw.WritePropertyName("unityVersion"); jw.WriteValue(Application.unityVersion);
				jw.WritePropertyName("fsms");
				jw.WriteStartArray();
				foreach (var fsm in all)
				{
					if (fsm == null) continue;
					GameObject go;
					try { go = fsm.gameObject; } catch { continue; }
					if (go == null) continue;
					// Prefab assets sit in an unloaded pseudo-scene; they are dumped too (`asset`), counted
					// apart, so a prefab's FSMs can be checked against the asset extraction
					// (tools/extract_assets.py).
					bool asset = !go.scene.isLoaded;
					_asset = asset;
					JObject o;
					try { o = One(fsm, go); }
					catch (Exception e)
					{
						ActionErrorCount++;
						o = new JObject
						{
							["path"] = ReflectionDumper.Path(go.transform),
							["__error"] = HKOracle.DescribeException(e),
						};
					}
					o["asset"] = asset;
					o.WriteTo(jw);
					if (asset) AssetFsmCount++; else FsmCount++;
				}
				jw.WriteEndArray();
				jw.WritePropertyName("counts");
				new JObject
				{
					["fsms"] = FsmCount,
					["assetFsms"] = AssetFsmCount,
					["assetActions"] = AssetActionCount,
					["actions"] = ActionCount,
					["distinctActionTypes"] = ActionTypes.Count,
					["actionErrors"] = ActionErrorCount,
					["unserialized"] = UnserializedCount,
					["initDataForced"] = InitDataCount,
				}.WriteTo(jw);
				jw.WriteEndObject();
				jw.Flush();
				sw.Flush();
			}
			long len = new System.IO.FileInfo(path).Length;
			Log($"wrote {path} ({len} bytes) fsms={FsmCount} actions={ActionCount} "
				+ $"types={ActionTypes.Count} errors={ActionErrorCount} unserialized={UnserializedCount} "
				+ $"initDataForced={InitDataCount}");
			if (len > 50L * 1024 * 1024)
				Log($"fsm.json exceeds 50MB ({len} bytes) — kept as one file; gzip downstream if needed");
			try { DumpGlobals(sceneDir); } catch (Exception e) { Log($"DumpGlobals FAILED: {HKOracle.DescribeException(e)}"); }
		}

		// Process-wide PlayMakerGlobals variable store + global event list.
		private static void DumpGlobals(string sceneDir)
		{
			var g = PlayMakerGlobals.Instance;
			var o = new JObject { ["present"] = g != null };
			if (g != null)
			{
				o["variables"] = Variables(g.Variables);
				var evs = new JArray();
				if (g.Events != null) foreach (var e in g.Events) evs.Add(e);
				o["events"] = evs;
			}
			string path = System.IO.Path.Combine(sceneDir, "globals.json");
			System.IO.File.WriteAllText(path, o.ToString(Formatting.Indented), new System.Text.UTF8Encoding(false));
			Log($"wrote {path}");
		}

		private static JObject One(PlayMakerFSM comp, GameObject go)
		{
			var o = new JObject
			{
				["path"] = ReflectionDumper.Path(go.transform),
				["gameObject"] = go.name,
				["goInstanceID"] = go.GetInstanceID(),
				["scene"] = go.scene.name,
				["instanceID"] = comp.GetInstanceID(),
				["activeInHierarchy"] = go.activeInHierarchy,
				["activeSelf"] = go.activeSelf,
				["enabled"] = comp.enabled,
				["isActiveAndEnabled"] = comp.isActiveAndEnabled,
				["fsmName"] = comp.FsmName,
				["template"] = comp.FsmTemplate == null ? null : comp.FsmTemplate.name,
			};

			var f = comp.Fsm;
			if (f == null) { o["__error"] = "PlayMakerFSM.Fsm == null"; return o; }

			// An FSM on an object that has never been enabled has not run
			// Fsm.Init, so FsmState.fsm is null and FsmState.Actions cannot
			// deserialize (ActionData.CreateAction:665 requires state.Fsm).
			// Fsm.InitData (Fsm.cs:1621) is the idempotent data-load half of
			// Init: it resolves events/transitions and calls state.LoadActions,
			// but starts nothing. Forcing it is the only way to read a pooled
			// projectile's actions. The dump process exits right after.
			o["initializedBeforeDump"] = f.Initialized;
			if (!f.Initialized)
			{
				try { f.InitData(); InitDataCount++; }
				catch (Exception e) { o["initData__error"] = HKOracle.DescribeException(e); }
			}

			o["name"] = f.Name;
			o["description"] = f.Description;
			try { o["dataVersion"] = f.DataVersion; } catch { }
			try { o["preprocessed"] = f.Preprocessed; } catch { }
			try { o["startState"] = f.StartState; } catch { }
			try { o["activeState"] = f.ActiveState == null ? null : f.ActiveState.Name; } catch { }
			try { o["activeStateName"] = f.ActiveStateName; } catch { }
			try { o["fsmActive"] = f.Active; } catch { }
			try { o["started"] = f.Started; } catch { }
			try { o["finished"] = f.Finished; } catch { }
			try { o["handleFixedUpdate"] = f.HandleFixedUpdate; } catch { }
			try { o["handleLateUpdate"] = f.HandleLateUpdate; } catch { }
			// Runtime-read fields not covered by the public field set.
			try { o["restartOnEnable"] = f.RestartOnEnable; } catch (Exception e) { o["restartOnEnable__error"] = e.Message; }
			try { o["manualUpdate"] = f.ManualUpdate; } catch (Exception e) { o["manualUpdate__error"] = e.Message; }
			try { o["keepDelayedEventsOnStateExit"] = f.KeepDelayedEventsOnStateExit; } catch (Exception e) { o["keepDelayedEventsOnStateExit__error"] = e.Message; }
			try { o["maxLoopCountOverride"] = f.MaxLoopCountOverride; } catch (Exception e) { o["maxLoopCountOverride__error"] = e.Message; }
			try { o["maxLoopCount"] = f.MaxLoopCount; } catch (Exception e) { o["maxLoopCount__error"] = e.Message; }
			try { o["exposedEvents"] = f.ExposedEvents == null ? -1 : f.ExposedEvents.Count; } catch (Exception e) { o["exposedEvents__error"] = e.Message; }
			try { o["hasHost"] = f.Host != null; } catch (Exception e) { o["hasHost__error"] = e.Message; }
			try { o["subFsmCount"] = f.SubFsmList == null ? -1 : f.SubFsmList.Count; } catch (Exception e) { o["subFsmCount__error"] = e.Message; }
			try { o["usedInTemplate"] = f.UsedInTemplate != null; } catch (Exception e) { o["usedInTemplate__error"] = e.Message; }

			o["variables"] = Variables(f.Variables);

			var evs = new JArray();
			if (f.Events != null)
				foreach (var e in f.Events)
				{
					if (e == null) { evs.Add(JValue.CreateNull()); continue; }
					evs.Add(new JObject { ["name"] = e.Name, ["isGlobal"] = e.IsGlobal });
				}
			o["events"] = evs;

			o["globalTransitions"] = Transitions(f.GlobalTransitions);

			var states = new JArray();
			if (f.States != null)
				foreach (var st in f.States)
				{
					if (st == null) { states.Add(JValue.CreateNull()); continue; }
					states.Add(State(st, st == f.ActiveState));
				}
			o["states"] = states;
			return o;
		}

		private static JArray Transitions(FsmTransition[] trs)
		{
			var arr = new JArray();
			if (trs == null) return arr;
			foreach (var tr in trs)
			{
				if (tr == null) { arr.Add(JValue.CreateNull()); continue; }
				arr.Add(new JObject
				{
					["event"] = tr.EventName,
					["toState"] = tr.ToState,
					["linkStyle"] = tr.LinkStyle.ToString(),
					["isGlobal"] = tr.FsmEvent != null && tr.FsmEvent.IsGlobal,
				});
			}
			return arr;
		}

		// `live` == this is the FSM's ACTIVE state, so its actions are mid-flight and their private
		// running state (timers, counters, latches) is part of what SceneReady means.  See LiveFields.
		private static JObject State(FsmState st, bool live)
		{
			var o = new JObject
			{
				["name"] = st.Name,
				["isSequence"] = st.IsSequence,
				["isBreakpoint"] = st.IsBreakpoint,
				["transitions"] = Transitions(st.Transitions),
			};
			FsmStateAction[] acts = null;
			try { acts = st.Actions; }
			catch (Exception e)
			{
				ActionErrorCount++;
				o["actions__error"] = HKOracle.DescribeException(e);
				o["actionNames"] = new JArray(st.ActionData == null ? new string[0] : st.ActionData.ActionNames.ToArray());
				return o;
			}
			var arr = new JArray();
			if (acts != null)
			{
				for (int i = 0; i < acts.Length; i++)
				{
					var a = acts[i];
					if (a == null)
					{
						ActionErrorCount++;
						arr.Add(new JObject { ["index"] = i, ["__error"] = "null action (ActionData.CreateAction returned null)" });
						continue;
					}
					var t = a.GetType();
					if (_asset) AssetActionCount++;
					else { ActionCount++; ActionTypes.Add(t.FullName); }
					var ao = new JObject { ["index"] = i, ["type"] = t.FullName };
					try { ao["enabled"] = a.Enabled; } catch (Exception e) { ao["enabled__error"] = e.Message; }
					try { ao["fields"] = ActionFields(a, t); }
					catch (Exception e) { ActionErrorCount++; ao["__error"] = HKOracle.DescribeException(e); }
					if (live)
					{
						try { ao["liveFields"] = LiveFields(a, t); }
						catch (Exception e) { ActionErrorCount++; ao["liveFields__error"] = HKOracle.DescribeException(e); }
					}
					arr.Add(ao);
				}
			}
			o["actions"] = arr;
			return o;
		}

		// The private, non-serialized running state of an action that is CURRENTLY EXECUTING: PlayMaker's
		// Wait keeps its countdown in a private `timer`, iTween actions their elapsed time, WaitRandom its
		// drawn duration.  None of it is in the public field set ActionFields dumps, and without it a
		// simulator that re-enters the dumped state restarts every in-flight timer at zero (e.g. Gruz
		// Mother is dumped part-way through a 0.75s Wait).
		//
		// Only primitives are taken, and only for the one active state per FSM, so this adds a few dozen
		// values per FSM rather than reflecting over all ~24k actions in a scene.
		private static JArray LiveFields(FsmStateAction a, Type t)
		{
			var arr = new JArray();
			foreach (var fi in ReflectionDumper.Fields(t, typeof(FsmStateAction)))
			{
				if (fi.IsPublic) continue;                      // already in ActionFields
				var ft = fi.FieldType;
				if (!(ft.IsPrimitive || ft.IsEnum)) continue;   // component/object refs are not state we can restore
				var e = new JObject { ["name"] = fi.Name, ["type"] = ft.FullName ?? ft.Name };
				try { e["value"] = FsmVal(fi.GetValue(a), 0); }
				catch (Exception ex) { ActionErrorCount++; e["__error"] = HKOracle.DescribeException(ex); }
				arr.Add(e);
			}
			return arr;
		}

		private static JArray ActionFields(FsmStateAction a, Type t)
		{
			var arr = new JArray();
			foreach (var fi in t.GetFields(BindingFlags.Public | BindingFlags.Instance))
			{
				var e = new JObject
				{
					["name"] = fi.Name,
					["type"] = fi.FieldType.FullName ?? fi.FieldType.Name,
				};
				// Only when the field is inherited: almost all action fields are
				// declared on the action itself, and the key is costly per scene.
				if (fi.DeclaringType != t) e["declaringType"] = fi.DeclaringType?.FullName;
				try { e["value"] = FsmVal(fi.GetValue(a), 0); }
				catch (Exception ex) { ActionErrorCount++; e["__error"] = HKOracle.DescribeException(ex); }
				arr.Add(e);
			}
			return arr;
		}

		// ------------------------------------------------------------ variables

		private static JObject Variables(FsmVariables v)
		{
			var o = new JObject();
			if (v == null) return o;
			o["Float"] = VarList(v.FloatVariables);
			o["Int"] = VarList(v.IntVariables);
			o["Bool"] = VarList(v.BoolVariables);
			o["String"] = VarList(v.StringVariables);
			o["Vector2"] = VarList(v.Vector2Variables);
			o["Vector3"] = VarList(v.Vector3Variables);
			o["Rect"] = VarList(v.RectVariables);
			o["Quaternion"] = VarList(v.QuaternionVariables);
			o["Color"] = VarList(v.ColorVariables);
			o["GameObject"] = VarList(v.GameObjectVariables);
			o["Array"] = VarList(v.ArrayVariables);
			o["Enum"] = VarList(v.EnumVariables);
			o["Object"] = VarList(v.ObjectVariables);
			o["Material"] = VarList(v.MaterialVariables);
			o["Texture"] = VarList(v.TextureVariables);
			return o;
		}

		private static JArray VarList(IEnumerable vars)
		{
			var arr = new JArray();
			if (vars == null) return arr;
			foreach (NamedVariable nv in vars)
			{
				if (nv == null) { arr.Add(JValue.CreateNull()); continue; }
				arr.Add(FsmVal(nv, 0));
			}
			return arr;
		}

		// ---------------------------------------------------------------- values

		private static JToken FsmVal(object v, int depth)
		{
			if (v == null) return JValue.CreateNull();
			if (_rec > kMaxRecursion) return Unser(v.GetType(), "recursion-limit", null, 0);
			_rec++;
			try { return FsmValInner(v, depth); }
			catch (Exception e) { ActionErrorCount++; return new JObject { ["__error"] = HKOracle.DescribeException(e) }; }
			finally { _rec--; }
		}

		private static string VarName(NamedVariable nv)
		{
			string n = nv.Name;
			return string.IsNullOrEmpty(n) ? null : n;
		}

		private static JToken FsmValInner(object v, int depth)
		{
			var t = v.GetType();

			// FsmOwnerDefault (FsmOwnerDefault.cs:15-27) — not a NamedVariable.
			if (v is FsmOwnerDefault owner)
				return new JObject
				{
					["__fsm"] = "FsmOwnerDefault",
					["ownerOption"] = owner.OwnerOption.ToString(),
					["gameObject"] = FsmVal(owner.GameObject, depth + 1),
				};

			// FsmEvent (FsmEvent.cs:42-162)
			if (v is FsmEvent ev)
				return new JObject { ["__fsm"] = "FsmEvent", ["name"] = ev.Name, ["isGlobal"] = ev.IsGlobal };

			if (v is NamedVariable nv)
			{
				var o = new JObject { ["__fsm"] = t.Name, ["name"] = VarName(nv) };
				try { o["useVariable"] = nv.UseVariable; } catch { }
				switch (v)
				{
					case FsmFloat f: o["value"] = f.Value; break;
					case FsmInt i: o["value"] = i.Value; break;
					case FsmBool b: o["value"] = b.Value; break;
					case FsmString s: o["value"] = s.Value; break;
					case FsmVector2 v2: o["value"] = ReflectionDumper.Val(v2.Value, 1); break;
					case FsmVector3 v3: o["value"] = ReflectionDumper.Val(v3.Value, 1); break;
					case FsmColor c: o["value"] = ReflectionDumper.Val(c.Value, 1); break;
					case FsmRect r: o["value"] = ReflectionDumper.Val(r.Value, 1); break;
					case FsmQuaternion q: o["value"] = ReflectionDumper.Val(q.Value, 1); break;
					case FsmGameObject g:
						o["value"] = g.Value == null ? (JToken)JValue.CreateNull() : ReflectionDumper.UnityRef(g.Value);
						break;
					case FsmEnum en:
						o["enumType"] = en.EnumType == null ? null : en.EnumType.FullName;
						o["enumName"] = en.EnumName;
						o["value"] = en.Value == null ? (JToken)JValue.CreateNull() : ReflectionDumper.Val(en.Value, 1);
						break;
					case FsmArray a:
						o["elementType"] = a.ElementType.ToString();
						o["objectTypeName"] = a.ObjectTypeName;
						var vals = new JArray();
						try { foreach (var el in a.Values) vals.Add(FsmVal(el, depth + 1)); }
						catch (Exception e) { vals.Add(new JObject { ["__error"] = e.Message }); }
						o["values"] = vals;
						break;
					// FsmMaterial / FsmTexture derive from FsmObject
					// (FsmMaterial.cs:9, FsmTexture.cs:9).
					case FsmObject ob:
						o["type"] = ob.TypeName;
						o["value"] = ob.Value == null ? (JToken)JValue.CreateNull() : ReflectionDumper.UnityRef(ob.Value);
						break;
					default:
						try { o["value"] = FsmVal(nv.RawValue, depth + 1); }
						catch (Exception e) { o["value__error"] = e.Message; }
						o["__unhandledNamedVariable"] = true;
						break;
				}
				return o;
			}

			if (v is string s2) return new JValue(s2);
			if (t.IsEnum || t.IsPrimitive || v is decimal
				|| v is Vector2 || v is Vector3 || v is Vector4 || v is Quaternion
				|| v is Color || v is Color32 || v is Rect || v is Bounds || v is LayerMask
				|| v is Type)
				return ReflectionDumper.Val(v, 1);

			if (v is UnityEngine.Object uo) return ReflectionDumper.UnityRef(uo);

			if (v is Array || v is IList)
			{
				var arr = new JArray();
				foreach (var el in (IEnumerable)v) arr.Add(FsmVal(el, depth));
				return arr;
			}

			// Everything else (FsmVar, FsmProperty, FsmEventTarget, FunctionCall,
			// FsmTemplateControl, LayoutOption, ...) is flagged __unserialized as
			// the contract requires, but its public+private instance fields are
			// still carried in __fields so no evidence is lost.
			return Unser(t, "unhandled-type", v, depth);
		}

		private static JObject Unser(Type t, string why, object v, int depth)
		{
			UnserializedCount++;
			var o = new JObject { ["__type"] = t.FullName, ["__unserialized"] = true, ["__reason"] = why };
			if (v == null || depth >= 1) return o;
			var fields = new JObject();
			foreach (var fi in ReflectionDumper.Fields(t, typeof(object)))
			{
				try { fields[fi.Name] = FsmVal(fi.GetValue(v), depth + 1); }
				catch (Exception e) { fields[fi.Name] = new JObject { ["__error"] = e.Message }; }
			}
			o["__fields"] = fields;
			return o;
		}
	}
}
