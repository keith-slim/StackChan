#pragma once

#include <cstdint>

namespace stackchan::stage1::weather {

enum class Kind {
    Unknown, Clear, Clouds, Drizzle, Rain, FreezingRain, Thunderstorm, Snow, Sleet,
    Mist, Fog, Smoke, Haze, Dust, Sand, Ash, Squall, Tornado
};

struct Condition {
    Kind kind = Kind::Unknown;
    uint8_t strength = 0;
    uint8_t clouds = 0;
    uint8_t lightning = 0;
    bool showers = false;
    const char* description = "不明な天気";
};

// OpenWeatherMapのコード分類を、表示・発話・Web状態で共有する。
constexpr Condition Classify(unsigned code) {
    using K = Kind;
    switch (code) {
    case 200: return {K::Thunderstorm,1,4,2,false,"小雨を伴う雷雨"};
    case 201: return {K::Thunderstorm,2,4,2,false,"雨を伴う雷雨"};
    case 202: return {K::Thunderstorm,4,4,3,false,"強い雨を伴う雷雨"};
    case 210: return {K::Thunderstorm,0,3,1,false,"弱い雷"};
    case 211: return {K::Thunderstorm,0,4,2,false,"雷"};
    case 212: return {K::Thunderstorm,0,4,3,false,"激しい雷"};
    case 221: return {K::Thunderstorm,0,4,2,true,"局地的な雷"};
    case 230: return {K::Thunderstorm,1,3,1,false,"弱い霧雨を伴う雷雨"};
    case 231: return {K::Thunderstorm,2,3,2,false,"霧雨を伴う雷雨"};
    case 232: return {K::Thunderstorm,3,4,3,false,"強い霧雨を伴う雷雨"};
    case 300: case 310: return {K::Drizzle,1,2,0,false,"弱い霧雨"};
    case 301: case 311: return {K::Drizzle,2,3,0,false,"霧雨"};
    case 302: case 312: return {K::Drizzle,3,4,0,false,"強い霧雨"};
    case 313: case 321: return {K::Drizzle,2,3,0,true,"にわか霧雨"};
    case 314: return {K::Drizzle,3,4,0,true,"強いにわか霧雨"};
    case 500: return {K::Rain,1,2,0,false,"小雨"};
    case 501: return {K::Rain,2,3,0,false,"雨"};
    case 502: return {K::Rain,3,4,0,false,"強い雨"};
    case 503: return {K::Rain,4,4,0,false,"非常に強い雨"};
    case 504: return {K::Rain,5,4,0,false,"猛烈な雨"};
    case 511: return {K::FreezingRain,2,4,0,false,"凍る雨"};
    case 520: return {K::Rain,1,2,0,true,"弱いにわか雨"};
    case 521: return {K::Rain,2,3,0,true,"にわか雨"};
    case 522: return {K::Rain,3,4,0,true,"強いにわか雨"};
    case 531: return {K::Rain,2,4,0,true,"局地的なにわか雨"};
    case 600: return {K::Snow,1,2,0,false,"小雪"};
    case 601: return {K::Snow,2,3,0,false,"雪"};
    case 602: return {K::Snow,4,4,0,false,"大雪"};
    case 611: return {K::Sleet,2,3,0,false,"みぞれ"};
    case 612: return {K::Sleet,1,2,0,true,"弱いにわかみぞれ"};
    case 613: return {K::Sleet,2,3,0,true,"にわかみぞれ"};
    case 615: return {K::Sleet,1,2,0,false,"弱い雨と雪"};
    case 616: return {K::Sleet,2,3,0,false,"雨と雪"};
    case 620: return {K::Snow,1,2,0,true,"弱いにわか雪"};
    case 621: return {K::Snow,2,3,0,true,"にわか雪"};
    case 622: return {K::Snow,4,4,0,true,"強いにわか雪"};
    case 701: return {K::Mist,1,0,0,false,"もや"};
    case 711: return {K::Smoke,2,0,0,false,"煙"};
    case 721: return {K::Haze,2,0,0,false,"かすみ"};
    case 731: return {K::Dust,3,0,0,true,"砂じんの渦"};
    case 741: return {K::Fog,3,0,0,false,"霧"};
    case 751: return {K::Sand,3,0,0,false,"砂"};
    case 761: return {K::Dust,2,0,0,false,"ほこり"};
    case 762: return {K::Ash,3,0,0,false,"火山灰"};
    case 771: return {K::Squall,4,3,0,false,"突風"};
    case 781: return {K::Tornado,5,4,0,false,"竜巻"};
    case 800: return {K::Clear,0,0,0,false,"晴れ"};
    case 801: return {K::Clouds,0,1,0,false,"雲が少ない天気"};
    case 802: return {K::Clouds,0,2,0,false,"雲が点在する天気"};
    case 803: return {K::Clouds,0,3,0,false,"雲が多い天気"};
    case 804: return {K::Clouds,0,4,0,false,"曇り"};
    default: return {};
    }
}

enum class Comfort { None, Hot, Cold };

struct Appearance {
    Condition condition;
    Comfort comfort = Comfort::None;
    bool visible = false;
    bool night = false;
};

inline Appearance MakeAppearance(unsigned code, double temperature, bool night, bool valid = true) {
    Appearance result;
    if (!valid) return result;
    result.condition = Classify(code);
    result.visible = result.condition.kind != Kind::Unknown;
    result.night = night;
    if (result.visible)
        result.comfort = temperature >= 30 ? Comfort::Hot :
                         temperature <= 10 ? Comfort::Cold : Comfort::None;
    return result;
}

} // namespace stackchan::stage1::weather
