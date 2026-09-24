using System;
using System.Collections.Generic;
using System.Globalization;
using Newtonsoft.Json.Linq;
using UnityEngine;

namespace HKOracle.Probe
{
	// One conformance scenario (spec format: hkpy/conformance.py, which runs the same spec through the sim).
	// Build() creates the scenario's objects under an inactive or active root at a fixed origin on a private
	// layer, the runner (ProbeDriver) then lets `shape.Length` frames pass, one character per frame: 'L' a live
	// frame (timeScale 1, one fixed step), 'F' a frozen one (timeScale 0).  Every probe callback in frames
	// [0, N) is logged as [f, stage, label, callback, other, x]; ops fire from triggers on probe callbacks
	// ("p") or on the driver's per-stage hooks ("at").
	public class Scenario
	{
		public static Scenario Current;

		private readonly JObject _spec;
		public readonly string Name;
		private readonly string _shape;
		private readonly Vector2 _origin;
		private readonly int _layer;
		public int Frame0;
		private bool _open;

		private readonly Dictionary<string, GameObject> _go = new Dictionary<string, GameObject>();
		private readonly Dictionary<string, ProbeBase> _probe = new Dictionary<string, ProbeBase>();
		private readonly JArray _events = new JArray(), _obs = new JArray(), _errors = new JArray(), _insts = new JArray();
		private readonly HashSet<ProbeBase> _known = new HashSet<ProbeBase>();
		private readonly Dictionary<string, int> _count = new Dictionary<string, int>();
		private readonly List<JObject> _triggers = new List<JObject>();
		private readonly HashSet<int> _fired = new HashSet<int>();
		private GameObject _root, _holder, _driverGo, _floorGo;
		private readonly List<string> _watch = new List<string>();
		private readonly Collider2D[] _buf = new Collider2D[32];

		public Scenario(JObject spec, Vector2 origin, int layer)
		{
			_spec = spec;
			Name = (string)spec["name"];
			_shape = (string)spec["shape"];
			_origin = origin;
			_layer = layer;
			if (spec["on"] is JArray on) foreach (JObject t in on) _triggers.Add(t);
			if (spec["watch"] is JArray w) foreach (var n in w) _watch.Add((string)n);
		}

		private int F => Time.frameCount - Frame0;
		private bool Logging => _open && F >= 0 && F < _shape.Length;

		// ------------------------------------------------------------------ build / teardown
		public void Build()
		{
			Frame0 = Time.frameCount + 1;
			_open = true;
			Current = this;

			_driverGo = new GameObject("conf_driver") { layer = _layer };
			_driverGo.transform.position = _origin + new Vector2(-100f, 0f);
			if ((bool?)_spec["phys_at"] == true)
			{
				// a permanent trigger overlap: the driver's physics-stage hook (OnTriggerEnter2D / Stay2D)
				var rb = _driverGo.AddComponent<Rigidbody2D>();
				rb.gravityScale = 0f; rb.sleepMode = RigidbodySleepMode2D.NeverSleep;
				rb.constraints = RigidbodyConstraints2D.FreezeRotation; rb.interpolation = RigidbodyInterpolation2D.None;
				var bc = _driverGo.AddComponent<BoxCollider2D>(); bc.isTrigger = true; bc.size = Vector2.one;
				_floorGo = new GameObject("conf_driver_floor") { layer = _layer };
				_floorGo.transform.position = _driverGo.transform.position;
				_floorGo.AddComponent<BoxCollider2D>().size = new Vector2(2f, 2f);
			}
			Drain((int?)_spec["drain"] ?? 0);
			var drv = _driverGo.AddComponent<ProbeDriverBehaviour>();
			drv.Sc = this;
			drv.StartLoops();

			_root = new GameObject("conf_root") { layer = _layer };
			_root.transform.position = _origin;
			_root.SetActive((bool?)_spec["root_active"] == true);
			_go["root"] = _root;
			_holder = new GameObject("conf_templates") { layer = _layer };
			_holder.SetActive(false);
			_holder.transform.position = _origin;
			if (_spec["templates"] is JArray tm) foreach (JObject o in tm) BuildObject(o, _holder);
			if (_spec["objects"] is JArray ob) foreach (JObject o in ob) BuildObject(o, null);
			SetTimeScaleFor(0);
		}

		// "drain": K.  Box2D hands out broadphase proxy ids from a LIFO free list (b2DynamicTree::AllocateNode /
		// FreeNode, analysis/upstream/box2d-v2.3.1/Box2D/Box2D/Collision/b2DynamicTree.cpp:53-99), so the ids the
		// scenario's colliders get depend on what the scene freed before, and the proxy ids order a contact's two
		// fixtures (b2BroadPhase.h QueryCallback, b2Min / b2Max) and same-step pairs (UpdatePairs' sort).  K static
		// colliders created here, before the scenario's objects, take every recently freed node, so the scenario's
		// proxies come from the tree's never-used tail, which AllocateNode links in ascending order: proxy ids
		// then ascend with fixture creation.  The drain colliders are never destroyed (a freed drain node would be
		// the next one handed out); they sit far below the scenario on its layer and touch nothing.
		private static GameObject _drainHolder;
		private static int _drained;

		private void Drain(int k)
		{
			if (k <= 0) return;
			if (_drainHolder == null)
			{
				_drainHolder = new GameObject("conf_drain") { layer = _layer };
				_drainHolder.transform.position = _origin + new Vector2(-300f, -300f);
			}
			for (int i = 0; i < k; i++, _drained++)
			{
				var d = new GameObject("d") { layer = _layer };
				d.transform.SetParent(_drainHolder.transform, false);
				d.transform.localPosition = new Vector2(2f * (_drained % 200), -2f * (_drained / 200));
				d.AddComponent<BoxCollider2D>().size = new Vector2(0.1f, 0.1f);
			}
			_obs.Add(new JArray(-1, "drain", k, _drained));
		}

		private void BuildObject(JObject o, GameObject defaultParent)
		{
			string name = (string)o["name"];
			var go = new GameObject(name) { layer = _layer };
			bool active = (bool?)o["active"] ?? true;
			if (!active) go.SetActive(false);
			string pn = (string)o["parent"];
			var parent = pn != null ? _go[pn] : (defaultParent ?? _root);
			go.transform.SetParent(parent.transform, false);
			go.transform.localPosition = V2(o["pos"], Vector2.zero);
			var sc = V2(o["scale"], Vector2.one);
			go.transform.localScale = new Vector3(sc.x, sc.y, 1f);
			go.transform.localRotation = Quaternion.Euler(0f, 0f, (float?)o["rot"] ?? 0f);
			_go[name] = go;
			if (o["body"] is JObject b)
			{
				var rb = go.AddComponent<Rigidbody2D>();
				rb.bodyType = BodyType((string)b["type"] ?? "dynamic");
				rb.gravityScale = (float?)b["gravity"] ?? 0f;
				rb.interpolation = RigidbodyInterpolation2D.None;
				rb.constraints = RigidbodyConstraints2D.FreezeRotation;
				string sm = (string)b["sleep"] ?? "start_awake";
				rb.sleepMode = sm == "never" ? RigidbodySleepMode2D.NeverSleep
					: sm == "start_asleep" ? RigidbodySleepMode2D.StartAsleep : RigidbodySleepMode2D.StartAwake;
				rb.collisionDetectionMode = (string)b["cd"] == "continuous"
					? CollisionDetectionMode2D.Continuous : CollisionDetectionMode2D.Discrete;
				if (b["vel"] != null) rb.velocity = V2(b["vel"], Vector2.zero);
			}
			if (o["colliders"] is JArray cols)
				foreach (JObject c in cols)
				{
					Collider2D col;
					if ((string)c["shape"] == "circle")
					{
						var cc = go.AddComponent<CircleCollider2D>();
						cc.radius = (float?)c["radius"] ?? 0.5f;
						col = cc;
					}
					else
					{
						var bc = go.AddComponent<BoxCollider2D>();
						bc.size = V2(c["size"], Vector2.one);
						col = bc;
					}
					col.offset = V2(c["offset"], Vector2.zero);
					col.isTrigger = (bool?)c["trigger"] ?? false;
					col.enabled = (bool?)c["enabled"] ?? true;
				}
			if (o["comps"] is JArray comps)
				for (int i = 0; i < comps.Count; i++)
				{
					var c = (JObject)comps[i];
					string kind = (string)c["kind"];
					var p = (ProbeBase)go.AddComponent(ProbeKinds.Types[kind]);
					p.Kind = kind;
					p.Label = name + ":" + i;
					if (c["q"] != null) p.Query = V2(c["q"], Vector2.zero);
					if ((bool?)c["enabled"] == false) p.enabled = false;
					Register(p);
				}
		}

		private void Register(ProbeBase p)
		{
			if (!_known.Add(p)) return;
			_probe[LabelOf(p)] = p;
			_insts.Add(new JObject
			{
				["label"] = LabelOf(p), ["kind"] = p.Kind, ["iid"] = p.GetInstanceID(),
				["go_iid"] = p.gameObject.GetInstanceID(), ["first_label"] = LabelOf(p),
			});
		}

		public void Close()
		{
			_open = false;
			if (Current == this) Current = null;
		}

		public void Teardown()
		{
			foreach (var g in new[] { _root, _holder, _driverGo, _floorGo })
				if (g != null) UnityEngine.Object.Destroy(g);
		}

		public JObject Result() => new JObject
		{
			["name"] = Name, ["spec"] = _spec, ["frame0"] = Frame0, ["layer"] = _layer,
			["origin"] = new JArray(_origin.x, _origin.y), ["insts"] = _insts, ["events"] = _events, ["obs"] = _obs,
			["errors"] = _errors,
		};

		// ------------------------------------------------------------------ labels
		// A probe's label: set at build / by the instantiate op; otherwise "<object name>:<index among the object's
		// probes>" (an Instantiate clone's own Awake / OnEnable run before the op can rename it).
		public static string LabelOf(ProbeBase p)
		{
			if (p.Label != null) return p.Label;
			var all = p.gameObject.GetComponents<ProbeBase>();
			int i = Array.IndexOf(all, p);
			return p.gameObject.name + ":" + i;
		}

		private static string NameOf(GameObject g) => g == null ? null : g.name;

		// ------------------------------------------------------------------ callbacks
		public void OnCallback(ProbeBase p, string cb, GameObject other, int k = -1)
		{
			if (!Logging) return;
			int f = F;
			string label = LabelOf(p);
			JToken x = null;
			if (k >= 0) x = k;
			else if (cb == "Awake" || cb == "OnEnable" || cb == "Start" || cb == "OnDisable" || cb == "OnDestroy")
			{
				var a = new JArray(p.gameObject.activeSelf, p.gameObject.activeInHierarchy, p.enabled);
				if (p.Query.HasValue)
				{
					var hit = Physics2D.OverlapPoint(_origin + p.Query.Value, 1 << _layer);
					a.Add(hit == null ? null : hit.gameObject.name);
				}
				x = a;
			}
			_events.Add(new JArray(f, ProbeStages.Current, label, cb, NameOf(other), x));
			string key = label + "|" + cb;
			_count.TryGetValue(key, out int n);
			_count[key] = ++n;
			for (int i = 0; i < _triggers.Count; i++)
			{
				var w = (JObject)_triggers[i]["when"];
				if (w["p"] == null || (string)w["p"] != label || (string)w["cb"] != cb) continue;
				if (w["f"] != null && (int)w["f"] != f) continue;
				if (w["n"] != null && (int)w["n"] != n) continue;
				if (w["k"] != null && (int)w["k"] != k) continue;
				Fire(i);
			}
		}

		public void At(string stage)
		{
			if (!Logging) return;
			int f = F;
			for (int i = 0; i < _triggers.Count; i++)
			{
				var w = (JObject)_triggers[i]["when"];
				if (w["at"] == null || (string)w["at"] != stage || (int)w["f"] != f) continue;
				Fire(i);
			}
			if (stage == "fixed_delayed") Watch(f);
		}

		public void EndOfLate(int nfixed)
		{
			int f = F;
			if (_open && f >= 0 && f < _shape.Length) _obs.Add(new JArray(f, "frame", Time.timeScale > 0f, nfixed));
			if (_open) SetTimeScaleFor(f + 1);
		}

		private void SetTimeScaleFor(int f) => Time.timeScale = f >= 0 && f < _shape.Length && _shape[f] == 'L' ? 1f : 0f;

		private void Watch(int f)
		{
			foreach (var n in _watch)
			{
				if (!_go.TryGetValue(n, out var g) || g == null) continue;
				var col = g.GetComponent<Collider2D>();
				if (col != null)
				{
					int m = col.GetContacts(_buf);
					var arr = new JArray();
					for (int i = 0; i < m; i++) arr.Add(NameOf(_buf[i].gameObject));
					_obs.Add(new JArray(f, "contacts", n, arr));
				}
				var rb = g.GetComponent<Rigidbody2D>();
				if (rb != null)
				{
					_obs.Add(new JArray(f, "awake", n, rb.IsAwake()));
					var pp = rb.position - _origin;
					_obs.Add(new JArray(f, "pos", n, new JArray(pp.x, pp.y)));
				}
			}
		}

		// ------------------------------------------------------------------ ops
		private void Fire(int i)
		{
			var t = _triggers[i];
			bool every = (bool?)t["every"] == true;
			if (!every && !_fired.Add(i)) return;
			foreach (JObject op in (JArray)t["do"])
			{
				try { Op(op); }
				catch (Exception e)
				{
					_errors.Add($"f={F} {op.ToString(Newtonsoft.Json.Formatting.None)}: {e.GetType().Name}: {e.Message}");
				}
			}
		}

		private void Mark(JObject op, string target, JToken x = null) =>
			_events.Add(new JArray(F, ProbeStages.Current, "op", (string)op["op"], target, x));

		private GameObject G(JObject op, string key = "go") => _go[(string)op[key]];
		private ProbeBase P(JObject op) => _probe[(string)op["p"]];
		private Rigidbody2D RB(JObject op) => G(op).GetComponent<Rigidbody2D>();
		private Collider2D Col(JObject op) => G(op).GetComponents<Collider2D>()[(int?)op["k"] ?? 0];

		private void Op(JObject op)
		{
			string kind = (string)op["op"];
			string tgt = (string)op["go"] ?? (string)op["p"];
			// the marker goes first: the callbacks the op causes follow it in the log
			if (kind != "overlap" && kind != "log_active") Mark(op, tgt);
			switch (kind)
			{
			case "set_active": G(op).SetActive((bool)op["v"]); break;
			case "enable": P(op).enabled = (bool)op["v"]; break;
			case "set_parent": G(op).transform.parent = G(op, "parent").transform; break;
			case "spawn":   // ObjectPool.orig_Spawn on a pooled object (ObjectPool.cs:487-497): parent, then SetActive
				G(op).transform.parent = G(op, "parent").transform;
				G(op).SetActive(true);
				break;
			case "instantiate": Instantiate(op); break;
			case "destroy":
				if (op["t"] != null) UnityEngine.Object.Destroy(G(op), (float)op["t"]);
				else UnityEngine.Object.Destroy(G(op));
				break;
			case "destroy_comp": UnityEngine.Object.Destroy(P(op)); break;
			case "start_co":
			{
				var p = P(op);
				var ys = new List<object>();
				foreach (var y in (JArray)op["ys"]) ys.Add(y.Type == JTokenType.String ? (object)(string)y : (float)y);
				p.StartCoroutine(p.Scripted((string)op["name"], ys));
				break;
			}
			case "stop_co": P(op).StopAllCoroutines(); break;
			case "log_active":
				Mark(op, tgt, new JArray(G(op).activeSelf, G(op).activeInHierarchy));
				break;
			case "overlap":
			{
				var hit = Physics2D.OverlapPoint(_origin + V2(op["pt"], Vector2.zero), 1 << _layer);
				Mark(op, null, NameOf(hit == null ? null : hit.gameObject));
				break;
			}
			case "vel": RB(op).velocity = V2(op["v"], Vector2.zero); break;
			case "pos":
				if ((string)op["via"] == "transform") G(op).transform.position = _origin + V2(op["p"], Vector2.zero);
				else RB(op).position = _origin + V2(op["p"], Vector2.zero);
				break;
			case "rot": RB(op).rotation = (float)op["deg"]; break;
			case "col_enabled": Col(op).enabled = (bool)op["v"]; break;
			case "trigger": Col(op).isTrigger = (bool)op["v"]; break;
			case "size": ((BoxCollider2D)Col(op)).size = V2(op["size"], Vector2.one); break;
			case "scale":
			{
				var s = V2(op["s"], Vector2.one);
				G(op).transform.localScale = new Vector3(s.x, s.y, 1f);
				break;
			}
			case "btype": RB(op).bodyType = BodyType((string)op["t"]); break;
			case "sleep": RB(op).Sleep(); break;
			case "wake": RB(op).WakeUp(); break;
			case "simulated": RB(op).simulated = (bool)op["v"]; break;
			default: throw new ArgumentException("unknown op " + kind);
			}
		}

		// Object.Instantiate(src, parent): the clone keeps src's local pose and activeSelf; its own Awake / OnEnable
		// run inside the call under the default labels ("<src>(Clone):i"), then the clone is renamed and relabelled
		// "<name>[/<child path>]:i".
		private void Instantiate(JObject op)
		{
			var src = G(op, "src");
			var parent = op["parent"] != null ? G(op, "parent") : _root;
			string name = (string)op["name"];
			var clone = UnityEngine.Object.Instantiate(src, parent.transform, false);
			clone.name = name;
			_go[name] = clone;
			foreach (var p in clone.GetComponentsInChildren<ProbeBase>(true))
			{
				string first = LabelOf(p);
				p.Kind = ProbeKindOf(p);
				string rel = p.transform == clone.transform ? name : name + "/" + RelPath(clone.transform, p.transform);
				if (p.transform != clone.transform) _go[rel] = p.gameObject;
				var all = p.gameObject.GetComponents<ProbeBase>();
				p.Label = rel + ":" + Array.IndexOf(all, p);
				p.Query = null;
				if (_known.Add(p))
				{
					_probe[p.Label] = p;
					_insts.Add(new JObject
					{
						["label"] = p.Label, ["kind"] = p.Kind, ["iid"] = p.GetInstanceID(),
						["go_iid"] = p.gameObject.GetInstanceID(), ["first_label"] = first,
					});
				}
			}
		}

		private static string ProbeKindOf(ProbeBase p)
		{
			foreach (var kv in ProbeKinds.Types) if (kv.Value == p.GetType()) return kv.Key;
			return "?";
		}

		private static string RelPath(Transform root, Transform t)
		{
			string s = t.name;
			for (var q = t.parent; q != null && q != root; q = q.parent) s = q.name + "/" + s;
			return s;
		}

		private static RigidbodyType2D BodyType(string t) =>
			t == "static" ? RigidbodyType2D.Static : t == "kinematic" ? RigidbodyType2D.Kinematic : RigidbodyType2D.Dynamic;

		private static Vector2 V2(JToken t, Vector2 dflt) =>
			t == null || t.Type == JTokenType.Null ? dflt : new Vector2((float)t[0], (float)t[1]);
	}
}
