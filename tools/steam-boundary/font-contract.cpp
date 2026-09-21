#include <windows.h>
#include <dwrite_2.h>
#include <cstdio>
#include <cstring>

#ifdef _WIN64
#define MACHINE "AMD64"
#define RESULT_FILE "C:\\font-contract-amd64.jsonl"
#else
#define MACHINE "I386"
#define RESULT_FILE "C:\\font-contract-i386.jsonl"
#endif

class Source final : public IDWriteTextAnalysisSource {
public:
    LONG refs = 1;
    unsigned calls = 0;
    const WCHAR *text;
    UINT32 length;
    Source(const WCHAR *s) : text(s), length((UINT32)wcslen(s)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDWriteTextAnalysisSource)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement(&refs); }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, const WCHAR **s, UINT32 *n) override {
        ++calls; *s = pos < length ? text + pos : nullptr;
        *n = pos < length ? length - pos : 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, const WCHAR **s, UINT32 *n) override {
        ++calls; *s = pos && pos <= length ? text : nullptr;
        *n = pos <= length ? pos : 0; return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        ++calls; return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32 *n, const WCHAR **locale) override {
        ++calls; *n = pos < length ? length - pos : 0; *locale = L"zh-CN"; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32 *n, IDWriteNumberSubstitution **sub) override {
        ++calls; *n = pos < length ? length - pos : 0; *sub = nullptr; return S_OK;
    }
};

int main(int argc, char **argv) {
    const char *summary = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--output")) summary = argv[++i];
    FILE *out = fopen(RESULT_FILE, "w");
    if (!out) return 2;
    setvbuf(out, nullptr, _IONBF, 0);
    fprintf(out, "{\"stage\":\"start\",\"machine\":\"%s\",\"pid\":%lu}\n", MACHINE, GetCurrentProcessId());
    // Exercise the same GDI-before-DirectWrite order as CEF system-font setup.
    HDC dc = GetDC(nullptr);
    if (dc) ReleaseDC(nullptr, dc);
    IDWriteFactory2 *factory = nullptr;
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown **)&factory);
    fprintf(out, "{\"stage\":\"factory\",\"hr\":%ld}\n", hr);
    if (FAILED(hr)) { fclose(out); return 3; }
    IDWriteFontCollection *collection = nullptr;
    IDWriteFontFallback *fallback = nullptr;
    hr = factory->GetSystemFontCollection(&collection, TRUE);
    fprintf(out, "{\"stage\":\"collection\",\"hr\":%ld,\"families\":%u}\n", hr,
            collection ? collection->GetFontFamilyCount() : 0);
    if (FAILED(hr)) { factory->Release(); fclose(out); return 4; }
    hr = factory->GetSystemFontFallback(&fallback);
    if (FAILED(hr)) { collection->Release(); factory->Release(); fclose(out); return 5; }
    struct Case { const char *label; const WCHAR *family; const WCHAR *text; };
    const Case cases[] = {
        {"tahoma-space", L"Tahoma", L" "},
        {"harmony-latin", L"\x9e3f\x8499\x9ed1\x4f53", L"ABC"},
        {"harmony-cjk", L"\x9e3f\x8499\x9ed1\x4f53", L"\x4e2d\x6587"},
        {"sans-space", L"sans", L" "}
    };
    bool ok = true;
    unsigned callbacks = 0;
    for (const auto &c : cases) {
        UINT32 index = 0; BOOL exists = FALSE;
        HRESULT find = collection->FindFamilyName(c.family, &index, &exists);
        Source source(c.text);
        bool passed = true; UINT32 mapped = 0; FLOAT scale = 0;
        HRESULT face_hr = E_FAIL;
        for (unsigned i = 0; i < 64; ++i) {
            IDWriteFont *font = nullptr;
            hr = fallback->MapCharacters(&source, 0, source.length, collection, c.family,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                &mapped, &font, &scale);
            IDWriteFontFace *face = nullptr;
            face_hr = font ? font->CreateFontFace(&face) : E_FAIL;
            passed = SUCCEEDED(hr) && mapped > 0 && mapped <= source.length &&
                     font && SUCCEEDED(face_hr) && face && scale > 0;
            if (face) face->Release();
            if (font) font->Release();
            if (!passed) break;
        }
        callbacks += source.calls;
        fprintf(out, "{\"stage\":\"case\",\"case\":\"%s\",\"find_hr\":%ld,\"exists\":%d,\"hr\":%ld,\"mapped\":%u,\"face_hr\":%ld,\"callbacks\":%u,\"pass\":%s}\n",
                c.label, find, exists, hr, mapped, face_hr, source.calls, passed ? "true" : "false");
        ok = ok && passed;
    }
    fprintf(out, "{\"stage\":\"result\",\"machine\":\"%s\",\"callbacks\":%u,\"status\":\"%s\",\"backend\":\"requires_runtime_log\"}\n",
            MACHINE, callbacks, ok ? "PASS" : "FAIL");
    fallback->Release(); collection->Release(); factory->Release(); fclose(out);
    if (summary) {
        FILE *result = fopen(summary, "w");
        if (!result) return 6;
        fprintf(result, "{\"schemaVersion\":1,\"status\":\"%s\",\"stage\":\"font-contract\",\"machine\":\"%s\",\"callbacks\":%u}\n",
                ok ? "PASS" : "FAIL", MACHINE, callbacks);
        fclose(result);
    }
    return ok ? 0 : 1;
}
