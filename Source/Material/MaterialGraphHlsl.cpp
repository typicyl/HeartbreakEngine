// Material/MaterialGraphHlsl.cpp - see MaterialGraphHlsl.h.
#include "Material/MaterialGraphHlsl.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace hbe::mat {

namespace {
std::string F(f32 v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.9g", static_cast<double>(v));
    std::string s = b;
    // HLSL float literal: ensure a decimal point so 3 isn't an int in float context.
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}
std::string V4(const glm::vec4& v) {
    return "float4(" + F(v.x) + "," + F(v.y) + "," + F(v.z) + "," + F(v.w) + ")";
}

// The fixed HLSL helper prelude (noise / colour / sdf / transform / blend), matching the CPU library
// closely (not bit-exact - this is an alternate GPU implementation).
const char* Prelude() {
    return R"HLSL(
float  mm_h1i(int2 c, float s){ return frac(sin(dot(float2(c)+s, float2(127.1,311.7)))*43758.5453); }
float2 mm_grad(int2 c, float s){ float a=mm_h1i(c,s)*6.2831853; return float2(cos(a),sin(a)); }
float  mm_perlin(float2 p, float s){
    int2 i=int2(floor(p)); float2 f=frac(p); float2 u=f*f*(3-2*f);
    float n00=dot(mm_grad(i,s),f), n10=dot(mm_grad(i+int2(1,0),s),f-float2(1,0));
    float n01=dot(mm_grad(i+int2(0,1),s),f-float2(0,1)), n11=dot(mm_grad(i+int2(1,1),s),f-float2(1,1));
    return saturate(lerp(lerp(n00,n10,u.x),lerp(n01,n11,u.x),u.y)*0.5+0.5);
}
float mm_fbm(float2 p, float oct, float pers, float s){
    float sum=0,a=0.5,t=0; int o=clamp((int)oct,1,8); float pr=(pers<=0?0.5:pers);
    for(int k=0;k<o;++k){ sum+=mm_perlin(p,s+k)*a; t+=a; p*=2; a*=pr; } return t>0?sum/t:0;
}
void mm_cell(float2 p, float s, out float f1, out float f2, out float2 cell){
    int2 c=int2(floor(p)); f1=8; f2=8; cell=float2(0,0);
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){ int2 g=c+int2(dx,dy);
        float2 ft=float2(g)+float2(mm_h1i(g,s),mm_h1i(g,s+13.7));
        float d=length(ft-p); if(d<f1){f2=f1;f1=d;cell=float2(g);} else if(d<f2) f2=d; } }
float3 mm_hsv2rgb(float3 c){ float4 K=float4(1,2.0/3.0,1.0/3.0,3); float3 p=abs(frac(c.xxx+K.xyz)*6-K.www); return c.z*lerp(K.xxx,saturate(p-K.xxx),c.y); }
float3 mm_rgb2hsv(float3 c){ float4 K=float4(0,-1.0/3.0,2.0/3.0,-1);
    float4 p=lerp(float4(c.bg,K.wz),float4(c.gb,K.xy),step(c.b,c.g));
    float4 q=lerp(float4(p.xyw,c.r),float4(c.r,p.yzx),step(p.x,c.r));
    float d=q.x-min(q.w,q.y); return float3(abs(q.z+(q.w-q.y)/(6*d+1e-10)), d/(q.x+1e-10), q.x); }
float mm_smin(float a,float b,float k){ if(k<=0)return min(a,b); float h=saturate(0.5+0.5*(b-a)/k); return lerp(b,a,h)-k*h*(1-h); }
float mm_sdpoly(float2 p,float r,int n){ if(n<3) return length(p)-r; float seg=6.2831853/n; float a=fmod(atan2(p.y,p.x),seg); if(a<0)a+=seg; a-=seg*0.5; return cos(a)*length(p)-r*cos(seg*0.5); }
float2 mm_xform(float2 uv,float4 c){ float s=(c.w==0?1:c.w); float2 p=(uv-0.5)/s; float ang=c.z*6.2831853; float ca=cos(ang),sa=sin(ang); return float2(p.x*ca-p.y*sa,p.x*sa+p.y*ca)+0.5-c.xy; }
float4 mm_div(float4 a,float4 b){ return a/select(abs(b)<1e-8,float4(1,1,1,1),b); }
float3 mm_blend(int m,float3 a,float3 b){
    if(m==1) return a*b; if(m==2) return 1-(1-a)*(1-b);
    if(m==3){ float3 r; for(int i=0;i<3;++i) r[i]=a[i]<0.5?2*a[i]*b[i]:1-2*(1-a[i])*(1-b[i]); return r; }
    if(m==4) return min(a,b); if(m==5) return max(a,b); if(m==6) return abs(a-b);
    if(m==7) return min(a+b,1); if(m==8) return max(a-b,0);
    if(m==9){ float3 r; for(int i=0;i<3;++i) r[i]=b[i]>=1?1:min(1.0,a[i]/(1-b[i])); return r; }
    if(m==10){ float3 r; for(int i=0;i<3;++i) r[i]=b[i]<=0?0:1-min(1.0,(1-a[i])/b[i]); return r; }
    if(m==12){ float3 r; for(int i=0;i<3;++i) r[i]=b[i]<0.5?2*a[i]*b[i]:1-2*(1-a[i])*(1-b[i]); return r; }
    return b;
}
)HLSL";
}
} // namespace

std::string GenerateComputeHlsl(const Graph& g) {
    const Node* outNode = g.OutputNode();
    std::ostringstream o;
    o << "// Generated from material graph \"" << g.name << "\" (MaterialGraphHlsl)\n";
    o << "[[vk::binding(0,0)]] cbuffer MMCB : register(b0){ uint gRes; uint gP0; uint gP1; uint gP2; };\n";
    o << "[[vk::binding(1,0)]] RWStructuredBuffer<float4> gOut : register(u0);\n";
    o << Prelude() << "\n";

    // Index nodes; reachability (backward BFS from Output); stable topo (Kahn).
    std::unordered_map<u32, const Node*> byId;
    for (const auto& n : g.nodes) byId[n.id] = &n;
    auto srcOf = [&](u32 nodeId, u8 pin) -> u32 {
        const Link* l = g.LinkInto(nodeId, pin);
        return (l && byId.count(l->fromNode)) ? l->fromNode : 0u;
    };

    std::vector<u32> order;
    if (outNode) {
        std::unordered_set<u32> reach;
        std::vector<u32> stk;
        auto pushInputs = [&](const Node& n) {
            const u8 ic = NodeInfoOf(n.type).inputCount;
            for (u8 p = 0; p < ic; ++p) {
                const u32 s = srcOf(n.id, p);
                if (s && !reach.count(s)) { reach.insert(s); stk.push_back(s); }
            }
        };
        pushInputs(*outNode);
        while (!stk.empty()) { const u32 id = stk.back(); stk.pop_back(); pushInputs(*byId[id]); }
        // Kahn, iterating node vector order for determinism.
        std::unordered_map<u32, int> indeg;
        for (const auto& n : g.nodes) {
            if (!reach.count(n.id)) continue;
            int d = 0;
            const u8 ic = NodeInfoOf(n.type).inputCount;
            for (u8 p = 0; p < ic; ++p) if (reach.count(srcOf(n.id, p))) ++d;
            indeg[n.id] = d;
        }
        std::unordered_set<u32> done;
        bool progress = true;
        while (progress) {
            progress = false;
            for (const auto& n : g.nodes) {
                if (!reach.count(n.id) || done.count(n.id) || indeg[n.id] != 0) continue;
                order.push_back(n.id);
                done.insert(n.id);
                progress = true;
                for (const auto& m : g.nodes) {
                    if (!reach.count(m.id) || done.count(m.id)) continue;
                    const u8 ic = NodeInfoOf(m.type).inputCount;
                    for (u8 p = 0; p < ic; ++p) if (srcOf(m.id, p) == n.id) indeg[m.id]--;
                }
            }
        }
    }

    // Per-node function bodies.
    for (const u32 id : order) {
        const Node& n = *byId[id];
        const glm::vec4 k = n.constant;
        // Input expression at a uv expression (value nodes use "uv"; transforms pass a modified uv).
        auto IN = [&](u8 pin, const char* uvExpr, const char* def) -> std::string {
            const u32 s = srcOf(id, pin);
            return s ? ("mm_" + std::to_string(s) + "(" + uvExpr + ")") : std::string(def);
        };
        o << "float4 mm_" << id << "(float2 uv){\n";
        switch (n.type) {
            case NodeType::Constant: case NodeType::Color: case NodeType::Vector:
                o << "  return " << V4(k) << ";\n"; break;
            case NodeType::Float: o << "  return float4(" << F(k.x) << "," << F(k.x) << "," << F(k.x) << "," << F(k.x) << ");\n"; break;
            case NodeType::UV: o << "  return float4(uv,0,0);\n"; break;
            case NodeType::WorldPosition: case NodeType::ObjectPosition: o << "  return float4(uv,0,1);\n"; break;
            case NodeType::Normal: o << "  return float4(0,0,1,0);\n"; break;
            case NodeType::VertexColor: o << "  return float4(1,1,1,1);\n"; break;
            case NodeType::Texture: o << "  return float4(1,1,1,1);\n"; break;         // no tex binding in preview
            case NodeType::NormalMap: o << "  return float4(0,0,1,1);\n"; break;
            case NodeType::Height: case NodeType::Mask: o << "  return " << F(k.x) << ".xxxx;\n"; break;
            case NodeType::MaterialLayer: o << "  return " << V4(k) << ";\n"; break;

            case NodeType::Multiply: o << "  return " << IN(0,"uv","float4(1,1,1,1)") << " * " << IN(1,"uv","float4(1,1,1,1)") << ";\n"; break;
            case NodeType::Add: o << "  return " << IN(0,"uv","float4(0,0,0,0)") << " + " << IN(1,"uv","float4(0,0,0,0)") << ";\n"; break;
            case NodeType::Subtract: o << "  return " << IN(0,"uv","float4(0,0,0,0)") << " - " << IN(1,"uv","float4(0,0,0,0)") << ";\n"; break;
            case NodeType::Divide: o << "  return mm_div(" << IN(0,"uv","float4(0,0,0,0)") << "," << IN(1,"uv","float4(1,1,1,1)") << ");\n"; break;
            case NodeType::Lerp: o << "  return lerp(" << IN(0,"uv","float4(0,0,0,0)") << "," << IN(1,"uv","float4(1,1,1,1)") << ",saturate(" << IN(2,"uv",(F(k.x)).c_str()) << ".x));\n"; break;
            case NodeType::Clamp: o << "  return clamp(" << IN(0,"uv","float4(0,0,0,0)") << "," << F(k.x) << "," << F(k.y) << ");\n"; break;
            case NodeType::Remap: o << "  float4 x=" << IN(0,"uv","float4(0,0,0,0)") << "; float sp=(" << F(k.y) << ")-(" << F(k.x) << "); float4 t=(abs(sp)<1e-8)?float4(0,0,0,0):(x-(" << F(k.x) << "))/sp; return (" << F(k.z) << ")+t*((" << F(k.w) << ")-(" << F(k.z) << "));\n"; break;
            case NodeType::Power: o << "  float4 x=max(" << IN(0,"uv","float4(0,0,0,0)") << ",0); float e=(" << F(k.x) << "==0?1:" << F(k.x) << "); x=(e<0?max(x,1e-6):x); return pow(x,e);\n"; break;
            case NodeType::Smoothstep: o << "  return smoothstep(" << F(k.x) << "," << F(k.y) << "," << IN(0,"uv","float4(0,0,0,0)") << ");\n"; break;
            case NodeType::OneMinus: o << "  return 1.0 - " << IN(0,"uv","float4(0,0,0,0)") << ";\n"; break;
            case NodeType::Noise: o << "  float2 c=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "; float v=mm_perlin(c*(" << F(k.x==0?1:k.x) << "),(" << F(k.y) << ")+1); return float4(v,v,v,1);\n"; break;
            case NodeType::Perlin: o << "  float2 c=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "; float v=mm_perlin(c*(" << F(k.x==0?1:k.x) << "),(" << F(k.y) << ")+1); return float4(v,v,v,1);\n"; break;
            case NodeType::FractalNoise: o << "  float2 c=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "; float v=mm_fbm(c*(" << F(k.x==0?1:k.x) << "),(" << F(k.y==0?5:k.y) << "),(" << F(k.z) << "),(" << F(k.w) << ")+1); return float4(v,v,v,1);\n"; break;
            case NodeType::Voronoi: case NodeType::Cellular: {
                const int mode = static_cast<int>(k.z);
                o << "  float2 c=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "; float f1,f2; float2 cell; mm_cell(c*(" << F(k.x==0?1:k.x) << "),(" << F(k.y) << ")+1,f1,f2,cell); float v="
                  << (mode==1?"min(f2,1.0)":mode==2?"saturate(f2-f1)":mode==3?"mm_h1i(int2(cell),(" + F(k.y) + ")+7)":"min(f1,1.0)") << "; return float4(v,v,v,1);\n"; break; }
            case NodeType::Checker: o << "  float2 c=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "*(" << F(k.x==0?1:k.x) << "); float v=(((int)floor(c.x)+(int)floor(c.y))&1)?1.0:0.0; return float4(v,v,v,1);\n"; break;
            case NodeType::Grid: o << "  float2 c=frac(" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "*(" << F(k.x==0?8:k.x) << ")); float lw=" << F(k.y==0?0.05f:k.y) << "; float v=(c.x<lw||c.x>1-lw||c.y<lw||c.y>1-lw)?1.0:0.0; return float4(v,v,v,1);\n"; break;
            case NodeType::Shape: o << "  float2 q=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "-0.5; float d=mm_sdpoly(q,(" << F(k.y==0?0.4f:k.y) << "),(int)(" << F(k.x) << ")); float aa=" << F(k.z>0?k.z:0.01f) << "; float v=1-smoothstep(-aa,aa,d); return float4(v,v,v,1);\n"; break;
            case NodeType::Wave: o << "  float x=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").x"):"uv.x") << "*(" << F(k.x==0?4:k.x) << ")+(" << F(k.z) << "); int ty=(int)(" << F(k.y) << "); float v=ty==1?1-abs(2*frac(x)-1):ty==2?frac(x):ty==3?(frac(x)<0.5?1.0:0.0):0.5+0.5*sin(x*6.2831853); return float4(v,v,v,1);\n"; break;
            case NodeType::Dots: o << "  float2 c=frac(" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "*(" << F(k.x==0?4:k.x) << "))-0.5; float r=" << F(k.y==0?0.3f:k.y) << "; float v=1-smoothstep(r-0.03,r,length(c)); return float4(v,v,v,1);\n"; break;
            case NodeType::RadialGradient: o << "  float v=saturate(1-length(" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "-0.5)/(" << F(k.x==0?0.5f:k.x) << ")); return float4(v,v,v,1);\n"; break;
            case NodeType::AngularGradient: o << "  float2 q=" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "-0.5; float v=atan2(q.y,q.x)/6.2831853+0.5; return float4(v,v,v,1);\n"; break;
            case NodeType::ColorRamp: {
                std::vector<RampStop> r = n.ramp;
                std::sort(r.begin(), r.end(), [](const RampStop&a,const RampStop&b){return a.pos<b.pos;});
                o << "  float t=saturate(" << IN(0,"uv","float4(0,0,0,0)") << ".x);\n";
                if (r.empty()) { o << "  return float4(t,t,t,1);\n"; }
                else {
                    o << "  float4 col=" << V4(r.front().color) << ";\n";
                    for (size_t si=1; si<r.size(); ++si) {
                        const float span = std::max(1e-4f, r[si].pos - r[si-1].pos);
                        o << "  col=lerp(col," << V4(r[si].color) << ",saturate((t-(" << F(r[si-1].pos) << "))/(" << F(span) << ")));\n";
                    }
                    o << "  return col;\n";
                }
                break; }

            case NodeType::Blend: o << "  float4 a=" << IN(0,"uv","float4(0,0,0,0)") << "; float4 b=" << IN(1,"uv","float4(0,0,0,0)") << "; float3 bl=mm_blend((int)(" << F(k.x) << "),a.rgb,b.rgb); return float4(lerp(a.rgb,bl,saturate(" << F(k.y==0?1:k.y) << ")),a.a);\n"; break;
            case NodeType::HSV: o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; float3 h=mm_rgb2hsv(c.rgb); h.x=frac(h.x+(" << F(k.x) << ")); h.y=saturate(h.y*(" << F(k.y==0?1:k.y) << ")); h.z=clamp(h.z*(" << F(k.z==0?1:k.z) << "),0,4); return float4(mm_hsv2rgb(h),c.a);\n"; break;
            case NodeType::BrightnessContrast: o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; return float4(saturate((c.rgb-0.5)*(" << F(k.y==0?1:k.y) << ")+0.5+(" << F(k.x) << ")),c.a);\n"; break;
            case NodeType::Levels: o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; float lo=" << F(k.x) << ",hi=(" << F(k.y==0?1:k.y) << "),ol=" << F(k.z) << ",oh=(" << F(k.w==0?1:k.w) << "); float3 t=saturate((c.rgb-lo)/max(hi-lo,1e-6)); return float4(ol+t*(oh-ol),c.a);\n"; break;
            case NodeType::Gamma: o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; return float4(pow(max(c.rgb,0),(" << F(k.x==0?1:k.x) << ")),c.a);\n"; break;
            case NodeType::Posterize: o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; float lv=(" << F(k.x<2?4:k.x) << "); return float4(saturate(floor(c.rgb*lv)/(lv-1)),c.a);\n"; break;
            case NodeType::Threshold: o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; float lum=dot(c.rgb,float3(0.299,0.587,0.114)); float v=lum>=(" << F(k.x==0?0.5f:k.x) << ")?1.0:0.0; return float4(v,v,v,c.a);\n"; break;
            case NodeType::Grayscale: o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; float v=dot(c.rgb,float3(0.299,0.587,0.114)); return float4(v,v,v,c.a);\n"; break;
            case NodeType::Combine: o << "  return float4(" << IN(0,"uv","float4(0,0,0,0)") << ".x," << IN(1,"uv","float4(0,0,0,0)") << ".x," << IN(2,"uv","float4(0,0,0,0)") << ".x," << (srcOf(id,3)?(IN(3,"uv","float4(1,1,1,1)")+".x"):"1.0") << ");\n"; break;
            case NodeType::Swizzle: { const int ch=static_cast<int>(k.x); o << "  float4 c=" << IN(0,"uv","float4(0,0,0,0)") << "; float v=" << (ch==1?"c.y":ch==2?"c.z":ch==3?"c.w":ch==4?"dot(c.rgb,float3(0.299,0.587,0.114))":"c.x") << "; return float4(v,v,v,v);\n"; break; }

            case NodeType::SdfCircle: o << "  return (length(" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "-0.5)-(" << F(k.x==0?0.3f:k.x) << ")).xxxx;\n"; break;
            case NodeType::SdfBox: o << "  float2 p=abs(" << (srcOf(id,0)?("(" + IN(0,"uv","float4(0,0,0,0)") + ").xy"):"uv") << "-0.5)-float2(" << F(k.x==0?0.3f:k.x) << "," << F(k.y==0?0.2f:k.y) << "); float d=length(max(p,0))+min(max(p.x,p.y),0); return d.xxxx;\n"; break;
            case NodeType::SdfOp: o << "  float a=" << IN(0,"uv","float4(1e9,0,0,0)") << ".x; float b=" << IN(1,"uv","float4(1e9,0,0,0)") << ".x; int m=(int)(" << F(k.x) << "); float d=m==1?max(a,-b):m==2?max(a,b):m==3?mm_smin(a,b,(" << F(k.y) << ")):min(a,b); return d.xxxx;\n"; break;
            case NodeType::SdfShow: o << "  float d=" << IN(0,"uv","float4(1e9,0,0,0)") << ".x; float v=1-smoothstep(0,(" << F(k.x==0?0.01f:k.x) << "),d); return float4(v,v,v,1);\n"; break;

            // Coordinate transforms: call input at a modified uv.
            case NodeType::Transform: o << "  return " << IN(0,("mm_xform(uv," + V4(k) + ")").c_str(),"float4(0,0,0,0)") << ";\n"; break;
            case NodeType::Tile: o << "  float2 t=float2(" << F(k.x==0?1:k.x) << "," << F(k.y==0?1:k.y) << "); return " << IN(0,"frac(uv*t)","float4(0,0,0,0)") << ";\n"; break;
            case NodeType::Mirror: { const int m=static_cast<int>(k.x); o << "  float2 mv=uv; float2 mm=abs(frac(uv*0.5)*2-1);";
                if(m==0||m==2) o << " mv.x=mm.x;"; if(m==1||m==2) o << " mv.y=mm.y;"; o << " return " << IN(0,"mv","float4(0,0,0,0)") << ";\n"; break; }
            case NodeType::Warp: o << "  float4 off=" << IN(1,"uv","float4(0.5,0.5,0.5,0.5)") << "; float2 wv=uv+(off.xy-0.5)*2*(" << F(k.x==0?0.1f:k.x) << "); return " << IN(0,"wv","float4(0,0,0,0)") << ";\n"; break;
            case NodeType::Kaleidoscope: o << "  float2 p=uv-0.5; float ang=atan2(p.y,p.x); float rr=length(p); float seg=6.2831853/max(1.0,floor(" << F(k.x==0?6:k.x) << ")); ang=abs(fmod(ang,seg)); ang=min(ang,seg-ang); return " << IN(0,"(float2(cos(ang),sin(ang))*rr+0.5)","float4(0,0,0,0)") << ";\n"; break;

            // Resampling filters: call input at offset uvs.
            case NodeType::HeightToNormal: o << "  float e=" << F(k.y>0?k.y:1.0f/256.0f) << "; float st=(" << F(k.x==0?1:k.x) << "); float hl=" << IN(0,"uv+float2(-e,0)","float4(0,0,0,0)") << ".x,hr=" << IN(0,"uv+float2(e,0)","float4(0,0,0,0)") << ".x,hd=" << IN(0,"uv+float2(0,-e)","float4(0,0,0,0)") << ".x,hu=" << IN(0,"uv+float2(0,e)","float4(0,0,0,0)") << ".x; float3 nn=normalize(float3(-(hr-hl)*st/(2*e),-(hu-hd)*st/(2*e),1)); return float4(nn,1);\n"; break;
            case NodeType::Blur: o << "  float rr=" << F(k.x>0?k.x:2.0f/256.0f) << "; float4 s=0; float w=0; for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){ float ww=(dx==0&&dy==0)?2.0:1.0; s+=" << IN(0,"uv+float2(dx,dy)*rr","float4(0,0,0,0)") << "*ww; w+=ww; } return s/w;\n"; break;
            case NodeType::AmbientOcclusion: o << "  float rr=" << F(k.y>0?k.y:4.0f/256.0f) << "; float st=(" << F(k.x==0?1:k.x) << "); float h0=" << IN(0,"uv","float4(0,0,0,0)") << ".x; float occ=0; int tp=0; for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){ if(dx==0&&dy==0)continue; occ+=max(0.0," << IN(0,"uv+float2(dx,dy)*rr","float4(0,0,0,0)") << ".x-h0); tp++; } float ao=1-saturate(occ/tp*st*4); return float4(ao,ao,ao,1);\n"; break;
            case NodeType::Emboss: o << "  float e=1.0/256.0; float d=(" << IN(0,"uv+float2(e,e)","float4(0,0,0,0)") << ".x-" << IN(0,"uv+float2(-e,-e)","float4(0,0,0,0)") << ".x)*(" << F(k.x==0?1:k.x) << "); float v=saturate(0.5+d); return float4(v,v,v,1);\n"; break;

            case NodeType::Bricks: {
                const std::string coord = srcOf(id, 0) ? ("(" + IN(0, "uv", "float4(0,0,0,0)") + ").xy") : std::string("uv");
                o << "  float2 c=" << coord << "; float cols=" << F(k.x == 0 ? 4 : k.x) << ",rows=" << F(k.y == 0 ? 8 : k.y)
                  << ",mo=" << F(k.z == 0 ? 0.05f : k.z) << ",ro=" << F(k.w == 0 ? 0.5f : k.w)
                  << "; float row=floor(c.y*rows); c.x+=row*ro/cols; float bx=frac(c.x*cols),by=frac(c.y*rows); float mh=mo*0.5;"
                  << " if(bx<mh||bx>1-mh||by<mh||by>1-mh) return float4(0,0,0,1);"
                  << " float rnd=mm_h1i(int2((int)floor(c.x*cols),(int)row),13.0); float v=0.5+0.5*rnd; return float4(v,v,v,1);\n";
                break;
            }
            case NodeType::Gradient:
                o << "  float t=saturate(" << (srcOf(id, 0) ? ("(" + IN(0, "uv", "float4(0,0,0,0)") + ").x") : std::string("uv.x"))
                  << "); return float4(t,t,t,1);\n";
                break;

            // Any node type without an explicit case above is a codegen gap: emit MAGENTA so it is
            // obvious in the preview (a silent passthrough returned black, which read as "broken").
            default: o << "  return float4(1,0,1,1);\n"; break;
        }
        o << "}\n";
    }

    // Channel evaluators (default when unbound).
    auto chExpr = [&](Channel c, const char* def) -> std::string {
        if (!outNode) return def;
        const u32 s = srcOf(outNode->id, static_cast<u8>(c));
        return s ? ("mm_" + std::to_string(s) + "(uv)") : std::string(def);
    };
    o << "\n[numthreads(8,8,1)]\nvoid CSMain(uint3 id : SV_DispatchThreadID){\n";
    o << "  if(id.x>=gRes||id.y>=gRes) return;\n";
    o << "  float2 uv=(float2(id.xy)+0.5)/gRes;\n";
    o << "  float4 base=" << chExpr(Channel::BaseColor, "float4(0.8,0.8,0.8,1)") << ";\n";
    o << "  float4 nrm=" << chExpr(Channel::Normal, "float4(0,0,1,1)") << ";\n";
    o << "  uint bi=(id.y*gRes+id.x)*8u;\n";
    o << "  gOut[bi+0]=float4(base.rgb," << "(" << chExpr(Channel::Opacity, "float4(1,1,1,1)") << ").x);\n";
    o << "  gOut[bi+1]=(" << chExpr(Channel::Roughness, "float4(0.5,0.5,0.5,1)") << ").xxxx;\n";
    o << "  gOut[bi+2]=(" << chExpr(Channel::Metallic, "float4(0,0,0,1)") << ").xxxx;\n";
    o << "  gOut[bi+3]=float4(normalize(nrm.xyz)*0.5+0.5,1);\n";
    o << "  gOut[bi+4]=(" << chExpr(Channel::Height, "float4(0.5,0.5,0.5,1)") << ").xxxx;\n";
    o << "  gOut[bi+5]=(" << chExpr(Channel::AO, "float4(1,1,1,1)") << ").xxxx;\n";
    o << "  gOut[bi+6]=" << chExpr(Channel::Emissive, "float4(0,0,0,1)") << ";\n";
    o << "  gOut[bi+7]=(" << chExpr(Channel::Opacity, "float4(1,1,1,1)") << ").xxxx;\n";
    o << "}\n";
    return o.str();
}

} // namespace hbe::mat
