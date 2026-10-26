#pragma once
// Coalesced 32-column tiles, eight adjacent 32-leaf subtrees per cell.
// One/two-row M2 Max profile; strict arithmetic and odd tails are unchanged.
// K<=16384 bounds the binary-counter roots below nine slots.
inline constexpr const char *hardwareSkinnyMatmulShader = R"MSL(
kernel void kalandra_matmul_hardware_skinny32(
 device KalandraHardwareMatrix& a [[buffer(0)]],
 device KalandraHardwareMatrix& b [[buffer(1)]],
 device KalandraHardwareMatrix& output [[buffer(2)]],
 device KalandraHardwareShape& parameters [[buffer(3)]],
    uint lane [[thread_position_in_threadgroup]],
    uint group [[threadgroup_position_in_grid]]) {
 threadgroup float partial[256];
 const uint m=parameters.values[0],k=parameters.values[1],n=parameters.values[2];
 const uint tiles=n/32+uint(n%32!=0),row=group%max(m,1u),col=(group/max(m,1u))*32+lane%32;
 const uint worker=lane/32,blocks=k/32,full=blocks/8;
 float stack[9];float tail=0.0f;
 for(uint panel=0;panel<full+uint(blocks%8!=0);++panel) {
   const uint block=panel*8+worker,first=block*32;
   if(row<m && col<n && block<blocks) {
     float p0=a.values[row*k+first+0]*b.values[(first+0)*n+col];
float p1=a.values[row*k+first+1]*b.values[(first+1)*n+col];
float p2=a.values[row*k+first+2]*b.values[(first+2)*n+col];
float p3=a.values[row*k+first+3]*b.values[(first+3)*n+col];
float p4=a.values[row*k+first+4]*b.values[(first+4)*n+col];
float p5=a.values[row*k+first+5]*b.values[(first+5)*n+col];
float p6=a.values[row*k+first+6]*b.values[(first+6)*n+col];
float p7=a.values[row*k+first+7]*b.values[(first+7)*n+col];
float p8=a.values[row*k+first+8]*b.values[(first+8)*n+col];
float p9=a.values[row*k+first+9]*b.values[(first+9)*n+col];
float p10=a.values[row*k+first+10]*b.values[(first+10)*n+col];
float p11=a.values[row*k+first+11]*b.values[(first+11)*n+col];
float p12=a.values[row*k+first+12]*b.values[(first+12)*n+col];
float p13=a.values[row*k+first+13]*b.values[(first+13)*n+col];
float p14=a.values[row*k+first+14]*b.values[(first+14)*n+col];
float p15=a.values[row*k+first+15]*b.values[(first+15)*n+col];
float p16=a.values[row*k+first+16]*b.values[(first+16)*n+col];
float p17=a.values[row*k+first+17]*b.values[(first+17)*n+col];
float p18=a.values[row*k+first+18]*b.values[(first+18)*n+col];
float p19=a.values[row*k+first+19]*b.values[(first+19)*n+col];
float p20=a.values[row*k+first+20]*b.values[(first+20)*n+col];
float p21=a.values[row*k+first+21]*b.values[(first+21)*n+col];
float p22=a.values[row*k+first+22]*b.values[(first+22)*n+col];
float p23=a.values[row*k+first+23]*b.values[(first+23)*n+col];
float p24=a.values[row*k+first+24]*b.values[(first+24)*n+col];
float p25=a.values[row*k+first+25]*b.values[(first+25)*n+col];
float p26=a.values[row*k+first+26]*b.values[(first+26)*n+col];
float p27=a.values[row*k+first+27]*b.values[(first+27)*n+col];
float p28=a.values[row*k+first+28]*b.values[(first+28)*n+col];
float p29=a.values[row*k+first+29]*b.values[(first+29)*n+col];
float p30=a.values[row*k+first+30]*b.values[(first+30)*n+col];
float p31=a.values[row*k+first+31]*b.values[(first+31)*n+col];
     partial[lane]=(((((p0+p1)+(p2+p3))+((p4+p5)+(p6+p7)))+(((p8+p9)+(p10+p11))+((p12+p13)+(p14+p15))))+((((p16+p17)+(p18+p19))+((p20+p21)+(p22+p23)))+(((p24+p25)+(p26+p27))+((p28+p29)+(p30+p31)))));
   }
   threadgroup_barrier(mem_flags::mem_threadgroup);
   if(worker==0 && row<m && col<n) {
     float roots[8];uint count=min(8u,blocks-panel*8);
     for(uint i=0;i<count;++i) roots[i]=partial[i*32+lane];
     while(count>1) {
       for(uint i=0;i<count/2;++i) roots[i]=roots[i*2]+roots[i*2+1];
       if(count&1) roots[count/2]=roots[count-1];count=(count+1)/2;
     }
     float value=roots[0];
     if(panel<full) {uint occupied=panel,level=0;
       while(occupied&1u) {value=stack[level]+value;++level;occupied>>=1;}
       stack[level]=value;
     } else tail=value;
   }
   threadgroup_barrier(mem_flags::mem_threadgroup);
 }
 if(worker==0 && row<m && col<n) {
   float value=tail;bool used=blocks%8!=0;
   for(uint level=0,occupied=full;occupied;++level,occupied>>=1) {
     if(occupied&1u) {value=used?stack[level]+value:stack[level];used=true;}
   }
   output.values[row*n+col]=value;
 }
}
)MSL";
