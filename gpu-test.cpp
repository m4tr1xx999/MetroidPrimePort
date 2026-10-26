#include <webgpu/webgpu_cpp.h>
#include "gfx/shadow_cascade.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Isolated evidence harness: the production CPU fit/box_mask maths against actual Metal
// rasterization. This intentionally does not claim to exercise the production caster queue,
// display lists, materials, or vs_shadow shader generation.
using namespace aurora::gfx::shadow::cascade;
namespace {
constexpr unsigned Edge = 1024;
constexpr unsigned Layers = Count;
constexpr unsigned ReadbackBytes = Edge * Edge * sizeof(float) * Layers;
constexpr unsigned UniformStride = 256;
struct Vertex { float p[3], uv[2], cut; };
using Transform = std::array<std::array<float, 4>, 3>;
struct Draw {
  unsigned first = 0;
  Transform model{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
  float lo[3], hi[3];
  bool unknown = false;
};
struct Scene { std::vector<Vertex> vertices; std::vector<Draw> draws; };
struct Uniform { float clip[4][4], model[3][4]; };
static_assert(sizeof(Uniform) == 112);

std::array<float, 3> world(const Transform& m, const float p[3]) {
  std::array<float, 3> out{};
  for (int a = 0; a < 3; ++a) out[a] = m[a][0]*p[0] + m[a][1]*p[1] + m[a][2]*p[2] + m[a][3];
  return out;
}

void add_triangle(Scene& scene, const std::array<std::array<float,3>,3>& positions, bool cut = false,
                  const Transform& model = {{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}}}, bool unknown = false) {
  Draw draw;
  draw.first = scene.vertices.size(); draw.model = model; draw.unknown = unknown;
  float localLo[3], localHi[3];
  std::fill_n(localLo,3,std::numeric_limits<float>::infinity());
  std::fill_n(localHi,3,-std::numeric_limits<float>::infinity());
  std::fill_n(draw.lo,3,std::numeric_limits<float>::infinity());
  std::fill_n(draw.hi,3,-std::numeric_limits<float>::infinity());
  for (unsigned i=0;i<3;++i) {
    Vertex v{};
    std::copy(positions[i].begin(),positions[i].end(),v.p);
    v.uv[0] = i==1; v.uv[1] = i==2; v.cut = cut;
    scene.vertices.push_back(v);
    for(int a=0;a<3;++a) {localLo[a]=std::min(localLo[a],v.p[a]);localHi[a]=std::max(localHi[a],v.p[a]);}
  }
  // Transform the full local AABB corners, as the room producer must do for rigid placements.
  for(unsigned bits=0;bits<8;++bits) {
    const float p[3]={bits&1?localHi[0]:localLo[0], bits&2?localHi[1]:localLo[1], bits&4?localHi[2]:localLo[2]};
    const auto q=world(model,p);
    for(int a=0;a<3;++a) {draw.lo[a]=std::min(draw.lo[a],q[a]);draw.hi[a]=std::max(draw.hi[a],q[a]);}
  }
  scene.draws.push_back(draw);
}

Scene make_scene(const Basis& basis, const Fit (&fits)[Count], unsigned variation) {
  Scene scene;
  std::mt19937 rng(81732+variation);
  std::uniform_real_distribution<float> pos(-220,220), delta(-25,25);
  for(unsigned t=0;t<800;++t) {
    std::array<std::array<float,3>,3> p{};
    float center[3]={pos(rng),pos(rng),pos(rng)};
    for(auto& v:p) for(int a=0;a<3;++a) v[a]=center[a]+delta(rng);
    add_triangle(scene,p,t%3==0,{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}},t%11==0);
  }
  // GPU-applied nonuniform scale, rotation, reflection and translation; their conservative
  // world-space bounds are independently built from every local AABB corner.
  constexpr std::array<std::array<float,3>,3> local{{{-12,-8,0},{13,-7,1},{1,15,-2}}};
  for(unsigned t=0;t<48;++t) {
    const float angle=t*0.191f, c=std::cos(angle), s=std::sin(angle);
    const float sx=t%2?-1.7f:1.7f, sy=t%3?0.8f:2.3f;
    const Transform m{{{c*sx,-s*sy,0,pos(rng)},{s*sx,c*sy,0,pos(rng)},{0,0,0.6f,pos(rng)}}};
    add_triangle(scene,local,t%2,m,t%13==0);
  }
  // Place triangles in each fitted layer's light space: at and just across lateral edges,
  // on/after its far plane, and far toward the sun (must survive near-plane pancaking).
  const auto toWorld=[&](float x,float y,float z) {
    std::array<float,3> p{};
    for(int a=0;a<3;++a) p[a]=x*basis.x[a]+y*basis.y[a]+z*basis.z[a];
    return p;
  };
  for(unsigned layer=0;layer<Layers;++layer) {
    const auto& f=fits[layer];
    const float x=(f.lo[0]+f.hi[0])*0.5f, y=(f.lo[1]+f.hi[1])*0.5f;
    const float midZ=(f.lo[2]+f.hi[2])*0.5f;
    const float width=(f.hi[0]-f.lo[0])/float(Edge);
    for(float edge:{f.lo[0],f.hi[0]}) for(float n:{-0.5f,0.f,0.5f}) {
      add_triangle(scene,{{toWorld(edge+n*width-width,y-1,midZ),toWorld(edge+n*width+width,y-1,midZ),toWorld(edge+n*width,y+1,midZ)}},true);
    }
    for(float edge:{f.lo[1],f.hi[1]}) for(float n:{-0.5f,0.f,0.5f}) {
      add_triangle(scene,{{toWorld(x-1,edge+n*width-width,midZ),toWorld(x+1,edge+n*width-width,midZ),toWorld(x,edge+n*width+width,midZ)}},true);
    }
    for(float z:{f.lo[2]-10000.f, f.lo[2], f.hi[2], f.hi[2]+width, f.hi[2]+100.f}) {
      add_triangle(scene,{{toWorld(x-3,y-2,z),toWorld(x+3,y-2,z),toWorld(x,y+3,z)}},layer%2);
    }
    // Cross the boundary with a single large primitive: no per-vertex rejection allowed.
    add_triangle(scene,{{toWorld(f.lo[0]-20,y-5,midZ),toWorld(f.hi[0]+20,y-5,midZ),toWorld(x,f.hi[1]+20,midZ)}},true);
  }
  return scene;
}

std::vector<std::string> gUncapturedErrors;

class Gpu {
public:
  Gpu() {
    instance=wgpu::CreateInstance();
    if(!instance) throw std::runtime_error("no WebGPU instance");
    bool done=false;
    wgpu::RequestAdapterOptions options{.backendType=wgpu::BackendType::Metal};
    instance.RequestAdapter(&options,wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::RequestAdapterStatus s,wgpu::Adapter a,wgpu::StringView message) {
        if(s==wgpu::RequestAdapterStatus::Success) adapter=std::move(a);
        else errors.emplace_back(std::string_view(message));
        done=true;
      });
    if(!until([&]{return done;})||!adapter) throw std::runtime_error("no Metal adapter");
    wgpu::DeviceDescriptor descriptor{};
    descriptor.SetUncapturedErrorCallback([](const wgpu::Device&,wgpu::ErrorType,wgpu::StringView message) {
      gUncapturedErrors.emplace_back(std::string_view(message));
    });
    done=false;
    adapter.RequestDevice(&descriptor,wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::RequestDeviceStatus s,wgpu::Device d,wgpu::StringView message) {
        if(s==wgpu::RequestDeviceStatus::Success) device=std::move(d);
        else errors.emplace_back(std::string_view(message));
        done=true;
      });
    if(!until([&]{return done;})||!device) throw std::runtime_error("no Metal device");
    queue=device.GetQueue();
    const char* source=R"(
      struct U { clipRows:array<vec4f,4>, modelRows:array<vec4f,3> };
      @group(0) @binding(0) var<uniform> u:U;
      struct O { @builtin(position) p:vec4f, @location(0) uv:vec2f, @location(1) cut:f32 };
      @vertex fn vs(@location(0) p:vec3f,@location(1) uv:vec2f,@location(2) cut:f32)->O {
        var o:O;
        let local=vec4f(p,1.0);
        let q=vec4f(dot(u.modelRows[0],local),dot(u.modelRows[1],local),dot(u.modelRows[2],local),1.0);
        o.p=vec4f(dot(u.clipRows[0],q),dot(u.clipRows[1],q),max(dot(u.clipRows[2],q),0.0),1.0);
        o.uv=uv; o.cut=cut; return o;
      }
      @fragment fn fs(o:O) {
        if(o.cut>0.5 && ((u32(o.uv.x*8.0)^u32(o.uv.y*8.0))&1u)!=0u) { discard; }
      })";
    wgpu::ShaderSourceWGSL wgsl;wgsl.code=source;
    wgpu::ShaderModuleDescriptor shaderDesc{.nextInChain=&wgsl};
    const auto shader=device.CreateShaderModule(&shaderDesc);
    const wgpu::VertexAttribute attrs[]={
      {.format=wgpu::VertexFormat::Float32x3,.offset=0,.shaderLocation=0},
      {.format=wgpu::VertexFormat::Float32x2,.offset=12,.shaderLocation=1},
      {.format=wgpu::VertexFormat::Float32,.offset=20,.shaderLocation=2}};
    const wgpu::VertexBufferLayout vb{.arrayStride=sizeof(Vertex),.attributeCount=3,.attributes=attrs};
    const wgpu::DepthStencilState depth{.format=wgpu::TextureFormat::Depth32Float,.depthWriteEnabled=wgpu::OptionalBool::True,.depthCompare=wgpu::CompareFunction::Less};
    const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs"};
    wgpu::RenderPipelineDescriptor pd;
    pd.vertex.module=shader;pd.vertex.entryPoint="vs";pd.vertex.bufferCount=1;pd.vertex.buffers=&vb;
    pd.depthStencil=&depth;pd.fragment=&fragment;
    // A reflected triangle is allowed to draw here. Production winding/cull behavior has its
    // own tests; this comparison isolates whether bounding-mask rejection changes rasterization.
    pd.primitive.cullMode=wgpu::CullMode::None;
    pipeline=device.CreateRenderPipeline(&pd);
  }

  ~Gpu() {if(device) device.Destroy();if(instance) instance.ProcessEvents();}
  template<typename P> bool until(P pred) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(!pred()&&std::chrono::steady_clock::now()<end) {
      instance.ProcessEvents();std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return pred();
  }

  std::vector<unsigned char> render(const Scene& scene,const Basis& basis,const Fit (&fits)[Count],bool filtered,
                                    std::array<unsigned,Count>& submitted) {
    wgpu::BufferDescriptor vd{.usage=wgpu::BufferUsage::Vertex|wgpu::BufferUsage::CopyDst,.size=scene.vertices.size()*sizeof(Vertex)};
    const auto vertices=device.CreateBuffer(&vd);queue.WriteBuffer(vertices,0,scene.vertices.data(),vd.size);
    std::vector<unsigned char> uniformBytes(scene.draws.size()*Layers*UniformStride,0);
    for(unsigned c=0;c<Layers;++c) for(unsigned d=0;d<scene.draws.size();++d) {
      Uniform u{};
      std::memcpy(u.clip,fits[c].clip,sizeof(u.clip));std::memcpy(u.model,scene.draws[d].model.data(),sizeof(u.model));
      std::memcpy(uniformBytes.data()+(c*scene.draws.size()+d)*UniformStride,&u,sizeof(u));
    }
    wgpu::BufferDescriptor ud{.usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=uniformBytes.size()};
    const auto uniforms=device.CreateBuffer(&ud);queue.WriteBuffer(uniforms,0,uniformBytes.data(),uniformBytes.size());
    std::vector<unsigned char> masks;
    for(const auto& draw:scene.draws) masks.push_back(draw.unknown?All:box_mask(basis,fits,draw.lo,draw.hi));
    const wgpu::TextureDescriptor td{.usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::CopySrc,.size={Edge,Edge,Layers},.format=wgpu::TextureFormat::Depth32Float};
    const auto texture=device.CreateTexture(&td);const auto encoder=device.CreateCommandEncoder();
    for(unsigned c=0;c<Layers;++c) {
      const wgpu::TextureViewDescriptor tv{.dimension=wgpu::TextureViewDimension::e2D,.baseArrayLayer=c,.arrayLayerCount=1};
      const wgpu::RenderPassDepthStencilAttachment da{.view=texture.CreateView(&tv),.depthLoadOp=wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=1.f};
      const wgpu::RenderPassDescriptor rp{.depthStencilAttachment=&da};
      const auto pass=encoder.BeginRenderPass(&rp);pass.SetPipeline(pipeline);pass.SetVertexBuffer(0,vertices);
      for(unsigned d=0;d<scene.draws.size();++d) {
        if(filtered&&!(masks[d]&(1u<<c))) continue;
        const wgpu::BindGroupEntry be{.binding=0,.buffer=uniforms,.offset=(c*scene.draws.size()+d)*UniformStride,.size=sizeof(Uniform)};
        const wgpu::BindGroupDescriptor bgd{.layout=pipeline.GetBindGroupLayout(0),.entryCount=1,.entries=&be};
        pass.SetBindGroup(0,device.CreateBindGroup(&bgd));pass.Draw(3,1,scene.draws[d].first);++submitted[c];
      }
      pass.End();
    }
    const wgpu::BufferDescriptor rd{.usage=wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::MapRead,.size=ReadbackBytes};
    const auto readback=device.CreateBuffer(&rd);
    const wgpu::TexelCopyTextureInfo from{.texture=texture,.aspect=wgpu::TextureAspect::DepthOnly};
    const wgpu::TexelCopyBufferInfo to{.layout={.bytesPerRow=Edge*sizeof(float),.rowsPerImage=Edge},.buffer=readback};
    const wgpu::Extent3D extent{Edge,Edge,Layers};encoder.CopyTextureToBuffer(&from,&to,&extent);
    const auto cmd=encoder.Finish();queue.Submit(1,&cmd);
    std::vector<unsigned char> out;bool done=false;
    readback.MapAsync(wgpu::MapMode::Read,0,ReadbackBytes,wgpu::CallbackMode::AllowProcessEvents,
      [&](wgpu::MapAsyncStatus status,wgpu::StringView message) {
        if(status==wgpu::MapAsyncStatus::Success) {
          const auto* data=static_cast<const unsigned char*>(readback.GetConstMappedRange());
          out.assign(data,data+ReadbackBytes);readback.Unmap();
        } else errors.emplace_back(std::string_view(message));
        done=true;
      });
    if(!until([&]{return done;})||out.size()!=ReadbackBytes) throw std::runtime_error("depth readback failed");
    if(!errors.empty()) throw std::runtime_error(errors.front());
    if(!gUncapturedErrors.empty()) throw std::runtime_error(gUncapturedErrors.front());
    return out;
  }

  wgpu::Instance instance;wgpu::Adapter adapter;wgpu::Device device;wgpu::Queue queue;wgpu::RenderPipeline pipeline;
  std::vector<std::string> errors;
};
} // namespace

int main() try {
  Gpu gpu;
  unsigned allTotal=0,filteredTotal=0;size_t coveredTotal=0;
  for(unsigned variation=0;variation<8;++variation) {
    const float suns[8][3]={{.3f,-.4f,-.8f},{-.7f,.3f,-.4f},{0,1,0},{0,0,1},{1,0,0},{-.1f,-.1f,-1.f},{.2f,.8f,-.5f},{.9f,-.2f,.1f}};
    Basis basis;if(!light_basis(suns[variation],basis)) throw std::runtime_error("invalid basis");
    const Frustum frustum{-.15f,.15f,.1f,-.1f,.2f,600.f};
    float split[Count];splits(frustum.znear,frustum.zfar,DefaultDistance,split);
    const float angle=variation*.215f,c=std::cos(angle),s=std::sin(angle);
    const float camera[3][4]={{c,-s,0,float(variation*13)},{s,c,0,-float(variation)*7.f},{0,0,1,float(variation*3)}};
    Fit fits[Count];fit(basis,camera,frustum,split,Edge,fits);
    const auto scene=make_scene(basis,fits,variation);
    std::array<unsigned,Count> all{},filtered{};
    const auto reference=gpu.render(scene,basis,fits,false,all);
    const auto optimized=gpu.render(scene,basis,fits,true,filtered);
    size_t changed=0,covered=0;
    for(size_t i=0;i<reference.size();i+=sizeof(float)) {
      float z;std::memcpy(&z,reference.data()+i,sizeof(z));
      if(!std::isfinite(z)||z<0.f||z>1.f) throw std::runtime_error("nonfinite or invalid reference depth");
      covered+=z<1.f;
      changed+=std::memcmp(reference.data()+i,optimized.data()+i,sizeof(float))!=0;
    }
    if(changed||covered<100) throw std::runtime_error("empty or mismatched depth map in case "+std::to_string(variation));
    for(unsigned cidx=0;cidx<Layers;++cidx) {allTotal+=all[cidx];filteredTotal+=filtered[cidx];}
    coveredTotal+=covered;
    std::printf("case=%u draws=%zu baseline=%u/%u/%u/%u filtered=%u/%u/%u/%u covered=%zu changed=0\n",variation,scene.draws.size(),all[0],all[1],all[2],all[3],filtered[0],filtered[1],filtered[2],filtered[3],covered);
    std::fflush(stdout);
  }
  if(filteredTotal>=allTotal) throw std::runtime_error("did not exercise any rejection");
  std::printf("PASS Metal GPU: 8 views x 4 layers x 1024x1024, depth bytes identical; %u -> %u submissions; %zu covered texels.\n",allTotal,filteredTotal,coveredTotal);
  std::puts("Coverage: opaque/cutout, unknown-All, GPU transforms/reflections, cascade side/far boundaries, sunward pancaking.\nScope: production fit/box_mask maths plus independent Metal rasterization; not the production caster queue or generated shaders.");
  return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
