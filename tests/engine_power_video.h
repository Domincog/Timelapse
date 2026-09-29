void pattern(lapse::Frame& out) {
    out.width = 320; out.height = 240; out.pixels.assign(320 * 240 * 4, 0);
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) {
        auto* p = out.pixels.data() + (size_t(y) * 320 + x) * 4;
        if (y < 120 && x < 160) p[2] = 255;
        else if (y < 120) p[1] = 255;
        else if (x < 160) p[0] = p[1] = p[2] = 255;
        p[3] = 255;
    }
}
void verify(const std::filesystem::path& file, unsigned frames) {
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader;
    checked(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader), "Cannot reopen recovered MP4");
    checked(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Deselect failed");
    checked(reader->SetStreamSelection(stream, TRUE), "Select failed");
    ComPtr<IMFMediaType> type;
    checked(reader->GetNativeMediaType(stream, 0, &type), "Missing native format");
    GUID subtype{}; checked(type->GetGUID(MF_MT_SUBTYPE, &subtype), "Missing codec");
    UINT32 w = 0, h = 0; checked(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h), "Missing size");
    require(subtype == MFVideoFormat_H264 && w == 320 && h == 240, "Recovered codec or dimensions wrong");
    PROPVARIANT duration; PropVariantInit(&duration);
    checked(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration), "Missing duration");
    const bool validDuration = duration.vt == VT_UI8 && std::llabs(static_cast<long long>(duration.uhVal.QuadPart) - frames * 10000000LL / 30) < 20000;
    PropVariantClear(&duration); require(validDuration, "Recovered duration wrong");
    type.Reset(); checked(MFCreateMediaType(&type), "Decoder type failed");
    checked(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Decoder major failed");
    checked(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "Decoder subtype failed");
    checked(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "NV12 decode unavailable");
    unsigned count = 0; bool eos = false;
    for (unsigned reads = 0; reads < frames + 100; ++reads) {
        DWORD flags = 0; LONGLONG timestamp = 0; ComPtr<IMFSample> sample;
        checked(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample), "Recovered decode failed");
        require(!(flags & MF_SOURCE_READERF_ERROR), "Recovered stream error");
        if (sample) {
            require(count < frames && std::llabs(timestamp - count * 10000000LL / 30) <= 1, "Recovered frame count/timing wrong");
            ComPtr<IMFMediaBuffer> buffer; ComPtr<IMF2DBuffer> plane;
            checked(sample->ConvertToContiguousBuffer(&buffer), "Missing decoded buffer");
            BYTE* bytes = nullptr; LONG stride = 320; DWORD length = 0;
            const bool twoD = SUCCEEDED(buffer.As(&plane));
            if (twoD) checked(plane->Lock2D(&bytes, &stride), "Cannot lock plane");
            else checked(buffer->Lock(&bytes, nullptr, &length), "Cannot lock buffer");
            bool valid = stride >= 320 && (twoD || length >= 320 * 240 * 3 / 2);
            const int points[][5] = {{80,60,63,102,240},{240,60,173,42,26},{80,180,235,128,128},{240,180,16,128,128}};
            if (valid) for (const auto& point : points) {
                const auto* uv = bytes + stride * 240 + (point[1] / 2) * stride + (point[0] & ~1);
                valid &= std::abs(int(bytes[point[1] * stride + point[0]]) - point[2]) <= 12 &&
                    std::abs(int(uv[0]) - point[3]) <= 12 && std::abs(int(uv[1]) - point[4]) <= 12;
            }
            if (twoD) plane->Unlock2D(); else buffer->Unlock();
            require(valid, "Recovered color/layout wrong"); ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eos = true; break; }
    }
    require(eos && count == frames, "Recovered decoded frame count wrong");
    std::cout << "  decoded=" << count << " bytes=" << std::filesystem::file_size(file) << '\n';
}
