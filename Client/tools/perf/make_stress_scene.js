// Builds a stress copy of the AkitaOnline Main scene (writes ONLY the given output scene file).
// usage: node make_stress_scene.js <input.scene> <output.scene>
const fs = require("fs");
const [input, output] = process.argv.slice(2);
if (!input || !output || input === output) throw new Error("usage: input output (different files)");

const scene = JSON.parse(fs.readFileSync(input, "utf8"));
let seed = 20261007;
const rand = () => ((seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296);
const range = (a, b) => a + (b - a) * rand();

const byName = (n) => scene.entities.find((e) => e.type === "mesh_entity" && e.name === n);
const player = byName("KicsiK");
const arissa = byName("Arissa");
const tree = byName("ujfa");
const cube = byName("Cube 3");
const clone = (o) => JSON.parse(JSON.stringify(o));
let nextId = 2000;

// 1) Characters: 31 more of each skinned model (32 per model is the engine's per-frame cap), in a
//    grid around the player.
const centre = player.position;
const characters = [];
for (let i = 0; i < 62; ++i) {
  const isArissa = i % 2 === 0;
  const e = clone(isArissa ? arissa : player);
  for (const k of ["rigidbody", "collider", "character_controller", "audio_listener", "script",
                   "particle_system", "audio_source", "hinge_joint", "prefab_asset_id", "prefab_instance"])
    delete e[k];
  e.id = nextId++;
  e.name = (isArissa ? "Crowd Arissa " : "Crowd KicsiK ") + i;
  const gx = (i % 8) - 3.5, gz = Math.floor(i / 8) - 3.5;
  e.position = [centre[0] + gx * 3.0 + range(-0.5, 0.5), isArissa ? 0 : centre[1], centre[2] + gz * 3.0 + range(-0.5, 0.5)];
  e.rotation = [isArissa ? 0 : 3.14157, range(-3.14, 3.14), isArissa ? 0 : 3.14159];
  characters.push(e);
}

// 2) Static props: 10 000 over 400 x 400 m — 30% trees, 70% boxes.
const props = [];
for (let i = 0; i < 10000; ++i) {
  const isTree = rand() < 0.3;
  const e = clone(isTree ? tree : cube);
  for (const k of ["rigidbody", "collider", "lod_component", "editor_components"]) delete e[k];
  e.id = nextId++;
  e.name = (isTree ? "Prop Tree " : "Prop Box ") + i;
  const s = isTree ? range(0.8, 1.3) : range(0.4, 2.0);
  e.position = [range(-200, 200), isTree ? -0.1 : s * 0.5, range(-200, 200)];
  e.rotation = [0, range(-3.14, 3.14), 0];
  e.scale = isTree ? [s, s, s] : [s, s * range(0.5, 1.5), s];
  props.push(e);
}

// 3) Particle emitters on the 100 boxes nearest the player: half CPU, half GPU.
const template = player.particle_system;
const near = props.filter((p) => p.name.startsWith("Prop Box"))
  .map((p) => ({ p, d: Math.hypot(p.position[0] - centre[0], p.position[2] - centre[2]) }))
  .sort((a, b) => a.d - b.d).slice(0, 100);
near.forEach(({ p }, i) => {
  const ps = clone(template);
  ps.gpu_simulation = i % 2 === 1;
  ps.max_particles = 200;
  ps.emission_rate = 50;
  ps.burst_count = 0;
  ps.soft_particles = false;
  ps.shape_radius = 0.8;
  p.particle_system = ps;
});

scene.entities.push(...characters, ...props);
fs.writeFileSync(output, JSON.stringify(scene, null, 2));
console.log(`wrote ${output}: ${characters.length} characters, ${props.length} props, ${near.length} emitters, ` +
  `${scene.entities.length} entities in all`);
