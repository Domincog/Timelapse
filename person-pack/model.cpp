// Pinned NanoDet-m320 decoder, adapted from the independently checked quality harness.
// RangiLyu NanoDet v0.4.0 geometry; Tencent/ncnn bilinear preprocessing.
// See NOTICE.txt for provenance and modifications. No external model path.
#include "model.h"
#include "resources.h"
#include <net.h>
#include <datareader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace lapse::person {
struct Detector::Impl { ncnn::Net net; bool ready = false; };
namespace {
class BoundedReader final : public ncnn::DataReader {
public:
    explicit BoundedReader(Resource resource) : data_(resource.data), bytes_(resource.bytes) {}
    size_t read(void* destination, size_t size) const override {
        if (size > bytes_ - position_) { failed_ = true; return 0; }
        std::memcpy(destination, data_ + position_, size); position_ += size; return size;
    }
    // Copying weights matches the reference FILE reader. It also avoids any
    // external-buffer alias or alignment dependency after initialization.
    size_t reference(size_t, const void**) const override { return 0; }
    bool complete() const noexcept { return !failed_ && position_ == bytes_; }
private:
    const uint8_t* data_; size_t bytes_; mutable size_t position_ = 0; mutable bool failed_ = false;
};
bool detailSufficient(const uint8_t* bgr, size_t bytes) noexcept {
    if (!bgr || !bytes || bytes > MaxBgrBytes || bytes % 3) return false;
    std::array<uint32_t, 256> histogram{}; uint64_t total = 0;
    const uint32_t count = uint32_t(bytes / 3);
    for (size_t at = 0; at < bytes; at += 3) {
        const unsigned luma = (77u * bgr[at + 2] + 150u * bgr[at + 1] + 29u * bgr[at] + 128u) >> 8;
        ++histogram[luma]; total += luma;
    }
    if (!count || total < uint64_t(12) * count || total > uint64_t(243) * count) return false;
    const uint32_t rank05 = (5u * count + 99u) / 100u, rank95 = (95u * count + 99u) / 100u;
    uint32_t cumulative = 0; int low = -1, high = -1;
    for (int value = 0; value < 256; ++value) {
        cumulative += histogram[size_t(value)];
        if (low < 0 && cumulative >= rank05) low = value;
        if (cumulative >= rank95) { high = value; break; }
    }
    return high - low >= 16;
}
static float clamp(float x,float low,float high){return (std::max)(low,(std::min)(high,x));}
static double distance(const float* logits) {
    const float maximum=*std::max_element(logits,logits+8);
    double sum=0,weighted=0;
    for(int k=0;k<8;++k){const double e=std::exp(double(logits[k]-maximum));sum+=e;weighted+=e*k;}
    const double result=weighted/sum;
    return std::isfinite(result)&&result>=0&&result<=7?result:-1;
}
static float area(const Box& b){return (b.x1-b.x0+1)*(b.y1-b.y0+1);}
static float overlap(const Box& a,const Box& b) {
    const float w=(std::max)(0.f,(std::min)(a.x1,b.x1)-(std::max)(a.x0,b.x0)+1);
    const float h=(std::max)(0.f,(std::min)(a.y1,b.y1)-(std::max)(a.y0,b.y0)+1);
    const float intersection=w*h,denominator=area(a)+area(b)-intersection;
    return denominator>0?intersection/denominator:0;
}
static int detect(ncnn::Net& net, const unsigned char* bgr, const Source& source, Box* boxes, Report* report) {
    *report={};
    try {
        const int rw = int(source.width), rh = int(source.height);
        const int left=(320-rw)/2,top=(320-rh)/2;
        report->resizedWidth=rw;report->resizedHeight=rh;report->padLeft=left;report->padTop=top;
        const auto resized=ncnn::Mat::from_pixels(bgr,ncnn::Mat::PIXEL_BGR,rw,rh);
        ncnn::Mat input;
        ncnn::copy_make_border(resized,input,top,320-rh-top,left,320-rw-left,ncnn::BORDER_CONSTANT,0.f);
        if(input.empty()||input.w!=320||input.h!=320||input.c!=3)return 2;
        const float means[3]={103.53f,116.28f,123.675f};
        const float norms[3]={0.017429f,0.017507f,0.017125f};
        input.substract_mean_normalize(means,norms);
        auto extractor=net.create_extractor();
        if(extractor.input("input.1",input)!=0)return 3;
        const char* names[3][2]={{"cls_pred_stride_8","dis_pred_stride_8"},{"cls_pred_stride_16","dis_pred_stride_16"},{"cls_pred_stride_32","dis_pred_stride_32"}};
        const int strides[3]={8,16,32};
        std::vector<Box> candidates;candidates.reserve(2100);
        for(int head=0;head<3;++head){
            ncnn::Mat cls,dis;
            if(extractor.extract(names[head][0],cls)!=0||extractor.extract(names[head][1],dis)!=0)return 4;
            const int stride=strides[head],grid=320/stride,rows=grid*grid;
            if(cls.dims!=2||cls.w!=80||cls.h!=rows||cls.elempack!=1||cls.elemsize!=4||
               dis.dims!=2||dis.w!=32||dis.h!=rows||dis.elempack!=1||dis.elemsize!=4)return 5;
            for(int at=0;at<rows;++at){
                const float* scores=cls.row(at);const float* logits=dis.row(at);int label=0;
                for(int k=0;k<80;++k){
                    if(!std::isfinite(scores[k])||scores[k]<0||scores[k]>1)return 6;
                    ++report->finiteClassValues;if(scores[k]>scores[label])label=k;
                }
                for(int k=0;k<32;++k){if(!std::isfinite(logits[k]))return 6;++report->finiteDistanceValues;}
                report->maxPersonHeadScore=(std::max)(report->maxPersonHeadScore,scores[0]);
                // Keep low-score decoded proposals for descriptive threshold
                // comparisons; the primary demo comparison uses score>=0.4.
                if(scores[label]<0.05f)continue;
                std::array<float,4> d{};
                for(int side=0;side<4;++side){const double v=distance(logits+side*8);if(v<0)return 7;d[size_t(side)]=float(v*stride);}
                const float cx=(float(at%grid)+.5f)*stride,cy=(float(at/grid)+.5f)*stride;
                Box box{clamp(cx-d[0],0,320),clamp(cy-d[1],0,320),clamp(cx+d[2],0,320),clamp(cy+d[3],0,320),scores[label],label};
                if(!std::isfinite(box.x0)||!std::isfinite(box.x1)||!std::isfinite(box.y0)||!std::isfinite(box.y1)||box.x1<=box.x0||box.y1<=box.y0)return 8;
                candidates.push_back(box);
            }
        }
        report->candidates=int(candidates.size());
        std::stable_sort(candidates.begin(),candidates.end(),[](const Box& a,const Box& b){return a.score>b.score;});
        std::vector<Box> kept;kept.reserve(candidates.size());
        for(const auto& candidate:candidates){
            bool suppressed=false;
            for(const auto& prior:kept)if(candidate.label==prior.label&&overlap(candidate,prior)>=0.5f){suppressed=true;break;}
            if(!suppressed)kept.push_back(candidate);
        }
        for(auto box:kept){
            // Match the model-release demo's separate effective-area ratios.
            if(!projectBox(box,source)){++report->invalidClipped;continue;}
            boxes[report->kept++]=box;
            if(box.label==0){++report->personKept;report->maxValidPersonScore=(std::max)(report->maxValidPersonScore,box.score);}
        }
        return 0;
    }catch(...){return 9;}
}

}
bool sufficientDetail(const uint8_t* bgr, size_t bytes) noexcept { return detailSufficient(bgr, bytes); }
bool projectBox(Box& box, const Source& source) noexcept {
    if (!validGeometry(source)) return false;
    const int width = int(source.sourceWidth), height = int(source.sourceHeight);
    const int rw = int(source.width), rh = int(source.height), left = (320 - rw) / 2, top = (320 - rh) / 2;
    box.x0=clamp((box.x0-left)*float(width)/rw,0,float(width-1));
    box.x1=clamp((box.x1-left)*float(width)/rw,0,float(width-1));
    box.y0=clamp((box.y0-top)*float(height)/rh,0,float(height-1));
    box.y1=clamp((box.y1-top)*float(height)/rh,0,float(height-1));
    return std::isfinite(box.x0) && std::isfinite(box.x1) && std::isfinite(box.y0) && std::isfinite(box.y1) &&
        box.x1 > box.x0 && box.y1 > box.y0;
}
Detector::Detector() : impl_(std::make_unique<Impl>()) {}
Detector::~Detector() = default;
bool Detector::initialize() noexcept {
    try {
        impl_->ready = false; impl_->net.clear();
        const Resource param = resource(ParamResource), weights = resource(WeightsResource);
        if (!param.data || param.bytes != 16741 || !weights.data || weights.bytes != 1901612) return false;
        std::string text(reinterpret_cast<const char*>(param.data), param.bytes);
        if (text.find('\0') != std::string::npos) return false;
        auto& option = impl_->net.opt;
        option.num_threads = 1; option.use_vulkan_compute = false;
        option.use_fp16_packed = false; option.use_fp16_storage = false; option.use_fp16_arithmetic = false;
        option.use_bf16_storage = false; option.use_int8_inference = true;
        if (impl_->net.load_param_mem(text.c_str()) != 0) return false;
        BoundedReader reader(weights);
        if (impl_->net.load_model(reader) != 0 || !reader.complete()) { impl_->net.clear(); return false; }
        impl_->ready = true; return true;
    } catch (...) { impl_->ready = false; return false; }
}
Output classify(const Report& report) noexcept {
    Output output{report.maxPersonHeadScore, report.maxValidPersonScore, Verdict::Unknown, Reason::InvalidOutput};
    if (!std::isfinite(output.rawMaxPerson) || !std::isfinite(output.validMaxPerson) ||
        output.rawMaxPerson < 0 || output.rawMaxPerson > 1 || output.validMaxPerson < 0 ||
        output.validMaxPerson > output.rawMaxPerson || report.finiteClassValues != 2100 * 80 ||
        report.finiteDistanceValues != 2100 * 32) return output;
    if (output.validMaxPerson >= PresentThreshold) { output.verdict = Verdict::Present; output.reason = Reason::None; }
    else if (output.rawMaxPerson <= AbsentThreshold) { output.verdict = Verdict::QualifiedAbsent; output.reason = Reason::None; }
    else output.reason = Reason::Ambiguous;
    return output;
}
bool Detector::infer(const uint8_t* bgr, size_t bytes, const Source& source, Output& output,
                     Report* diagnostic, Box* boxes, size_t capacity) noexcept {
    output = {0, 0, Verdict::Unknown, Reason::InvalidInput};
    if (diagnostic) *diagnostic = {};
    if (!bgr || !validGeometry(source) || bytes != size_t(source.width) * source.height * 3 ||
        bytes > MaxBgrBytes || (boxes && capacity < 2100)) return false;
    if (!impl_->ready) { output.reason = Reason::ModelFailure; return false; }
    std::array<Box, 2100> localBoxes{}; Report report{};
    const int result = detect(impl_->net, bgr, source, boxes ? boxes : localBoxes.data(), &report);
    if (diagnostic) *diagnostic = report;
    if (result) {
        output.reason = result == 6 || result == 7 || result == 8 ? Reason::InvalidOutput : Reason::ModelFailure;
        return false;
    }
    output = classify(report);
    if (output.verdict == Verdict::QualifiedAbsent && !sufficientDetail(bgr, bytes)) {
        output.verdict = Verdict::Unknown; output.reason = Reason::InsufficientDetail;
    }
    return true;
}
}
