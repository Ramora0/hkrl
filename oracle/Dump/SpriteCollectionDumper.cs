using System;
using System.Collections.Generic;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEngine;
using HKOracle.Env;

namespace HKOracle.Dump
{
	// sprites.json: the per-sprite COLLIDER geometry baked into every tk2d sprite collection the scene
	// can reach.
	//
	// Why this is not cosmetic data.  tk2dBaseSprite.spriteId's setter calls UpdateCollider()
	// (tk2dBaseSprite.cs:182), and the animator sets spriteId on every frame change
	// (tk2dSpriteAnimator.SetFrameInternal :551-557).  For a sprite whose definition says
	// physicsEngine == Physics2D and colliderType == Box, UpdateCollider writes the GameObject's
	// BoxCollider2D outright:
	//     offset = colliderVertices[0] * scale
	//     size   = |2 * colliderVertices[1] * scale|                (tk2dBaseSprite.cs:509-510)
	// so an enemy's hurtbox is ANIMATION-DRIVEN, and none of it is recoverable from the hierarchy dump,
	// which records only the box the collider happened to hold at SceneReady.  Bosses with no
	// collider action in their FSMs (False Knight, Mega Moss Charger) change hurtbox only this way.
	public static class SpriteCollectionDumper
	{
		public static int CollectionCount, SpriteCount;


		static JArray V2(Vector2[] p)
		{
			var a = new JArray();
			if (p != null) foreach (var v in p) a.Add(new JArray(v.x, v.y));
			return a;
		}

		static JObject Sprite(int id, tk2dSpriteDefinition d)
		{
			var o = new JObject
			{
				["id"] = id,
				["name"] = d.name,
				["physicsEngine"] = (int)d.physicsEngine,
				["colliderType"] = (int)d.colliderType,
			};
			// Box: exactly two vertices are read (centre, half-extent). Mesh: the polygon/edge paths.
			if (d.colliderType == tk2dSpriteDefinition.ColliderType.Box && d.colliderVertices != null && d.colliderVertices.Length >= 2)
			{
				o["colliderVertices"] = new JArray(
					new JArray(d.colliderVertices[0].x, d.colliderVertices[0].y, d.colliderVertices[0].z),
					new JArray(d.colliderVertices[1].x, d.colliderVertices[1].y, d.colliderVertices[1].z));
			}
			else if (d.colliderType == tk2dSpriteDefinition.ColliderType.Mesh)
			{
				var poly = new JArray();
				if (d.polygonCollider2D != null) foreach (var c in d.polygonCollider2D) poly.Add(V2(c == null ? null : c.points));
				o["polygonCollider2D"] = poly;
				var edge = new JArray();
				if (d.edgeCollider2D != null) foreach (var c in d.edgeCollider2D) edge.Add(V2(c == null ? null : c.points));
				o["edgeCollider2D"] = edge;
			}
			return o;
		}

		public static void Dump(string dir, string scene)
		{
			CollectionCount = SpriteCount = 0;
			var seen = new HashSet<int>();
			var arr = new JArray();
			foreach (var coll in Resources.FindObjectsOfTypeAll<tk2dSpriteCollectionData>())
			{
				if (coll == null) continue;
				// A sprite reads its definitions through `inst`, the platform-specific copy
				// (tk2dBaseSprite.InitInstance), so that is the object whose contents matter. Key the
				// dump by the OUTER collection's name + instanceID, because that is what the animator's
				// frames and the tk2dSprite component reference.
				tk2dSpriteCollectionData inst = null;
				try { inst = coll.inst; } catch { }
				if (inst == null || inst.spriteDefinitions == null) continue;
				if (!seen.Add(coll.GetInstanceID())) continue;
				var sprites = new JArray();
				for (int i = 0; i < inst.spriteDefinitions.Length; i++)
				{
					var d = inst.spriteDefinitions[i];
					if (d == null) { sprites.Add(JValue.CreateNull()); continue; }
					try { sprites.Add(Sprite(i, d)); }
					catch (Exception e) { sprites.Add(new JObject { ["id"] = i, ["__error"] = HKOracle.DescribeException(e) }); }
					SpriteCount++;
				}
				arr.Add(new JObject
				{
					["name"] = coll.name,
					["instanceID"] = coll.GetInstanceID(),
					["instInstanceID"] = inst.GetInstanceID(),
					["spriteCollectionName"] = inst.spriteCollectionName,
					["count"] = sprites.Count,
					["sprites"] = sprites,
				});
				CollectionCount++;
			}
			var o = new JObject { ["scene"] = scene, ["frame"] = Time.frameCount, ["collections"] = arr };
			string path = System.IO.Path.Combine(dir, "sprites.json");
			using (var sw = new System.IO.StreamWriter(path, false, new System.Text.UTF8Encoding(false)))
			using (var jw = new JsonTextWriter(sw) { Formatting = Formatting.None })
			{
				o.WriteTo(jw);
			}
			HKOracle.Instance.Log($"[Dump] sprites.json: {CollectionCount} collections, {SpriteCount} sprites -> {path}");
		}
	}
}
