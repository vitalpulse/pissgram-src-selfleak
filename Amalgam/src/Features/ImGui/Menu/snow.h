#pragma once
#include "../../../SDK/SDK.h"
#include <vector>
#include <random>
#include <ImGui/imgui.h>


struct Snowflake
{
    ImVec2 Position;
    ImVec2 Velocity;
    float Size;
    float Opacity;
    float SwayOffset;
    float SwaySpeed;
};

class CSnow
{
private:
    std::mt19937 m_RandomGen;
    std::uniform_real_distribution<float> m_DistX;
    std::uniform_real_distribution<float> m_DistY;
    std::uniform_real_distribution<float> m_DistSize;
    std::uniform_real_distribution<float> m_DistSpeed;
    std::uniform_real_distribution<float> m_DistOpacity;
    std::uniform_real_distribution<float> m_DistSway;

    float m_flLastTime;
    ImVec2 m_vLastMousePos;
    bool m_bInitialized;

    void InitializeSnowflake(Snowflake& flake, const ImVec2& displaySize);
    void UpdateSnowflake(Snowflake& flake, float deltaTime, const ImVec2& displaySize, const ImVec2& mousePos, const ImVec2& mouseDelta);

public:
    std::vector<Snowflake> m_vSnowflakes;

    CSnow();
    void Initialize(int count = 100);
    void Update(const ImVec2& displaySize);
    void Render();
    void Clear();
    bool IsInitialized() const { return m_bInitialized; }
};

ADD_FEATURE(CSnow, Snow)