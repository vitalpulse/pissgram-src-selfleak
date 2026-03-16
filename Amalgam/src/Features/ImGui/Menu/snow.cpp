#include "snow.h"

CSnow::CSnow()
    : m_RandomGen(std::random_device{}())
    , m_flLastTime(0.f)
    , m_vLastMousePos(ImVec2(0, 0))
    , m_bInitialized(false)
{
}

void CSnow::Initialize(int count)
{
    m_vSnowflakes.clear();
    m_vSnowflakes.reserve(count);

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    // Initialize random distributions
    m_DistX = std::uniform_real_distribution<float>(0.f, displaySize.x);
    m_DistY = std::uniform_real_distribution<float>(-displaySize.y, 0.f);
    m_DistSize = std::uniform_real_distribution<float>(2.f, 5.f);
    m_DistSpeed = std::uniform_real_distribution<float>(20.f, 60.f);
    m_DistOpacity = std::uniform_real_distribution<float>(0.3f, 0.9f);
    m_DistSway = std::uniform_real_distribution<float>(0.f, 6.28f); // 0 to 2*PI

    for (int i = 0; i < count; ++i)
    {
        Snowflake flake;
        InitializeSnowflake(flake, displaySize);
        m_vSnowflakes.push_back(flake);
    }

    m_flLastTime = ImGui::GetTime();
    m_bInitialized = true;
}

void CSnow::InitializeSnowflake(Snowflake& flake, const ImVec2& displaySize)
{
    flake.Position.x = m_DistX(m_RandomGen);
    flake.Position.y = m_DistY(m_RandomGen);
    flake.Size = m_DistSize(m_RandomGen);
    flake.Velocity.y = m_DistSpeed(m_RandomGen);
    flake.Velocity.x = 0.f;
    flake.Opacity = m_DistOpacity(m_RandomGen);
    flake.SwayOffset = m_DistSway(m_RandomGen);
    flake.SwaySpeed = m_DistSpeed(m_RandomGen) * 0.02f;
}

void CSnow::UpdateSnowflake(Snowflake& flake, float deltaTime, const ImVec2& displaySize, const ImVec2& mousePos, const ImVec2& mouseDelta)
{
    // Update sway offset
    flake.SwayOffset += flake.SwaySpeed * deltaTime;

    // Calculate horizontal sway
    float swayAmount = sin(flake.SwayOffset) * 30.f;

    // Mouse interaction - push snowflakes away
    float dx = flake.Position.x - mousePos.x;
    float dy = flake.Position.y - mousePos.y;
    float distSq = dx * dx + dy * dy;
    float interactionRadius = 100.f;
    float interactionRadiusSq = interactionRadius * interactionRadius;

    if (distSq < interactionRadiusSq && distSq > 0.f)
    {
        float dist = sqrt(distSq);
        float force = (1.f - dist / interactionRadius) * 200.f;

        // Normalize and apply force
        flake.Velocity.x += (dx / dist) * force * deltaTime;
        flake.Velocity.y += (dy / dist) * force * deltaTime;

        // Add mouse velocity influence
        flake.Velocity.x += mouseDelta.x * 0.5f;
        flake.Velocity.y += mouseDelta.y * 0.5f;
    }

    // Apply drag to return to normal falling
    flake.Velocity.x *= 0.95f;
    flake.Velocity.y = flake.Velocity.y * 0.98f + m_DistSpeed(m_RandomGen) * 0.02f;

    // Update position
    flake.Position.x += (flake.Velocity.x + swayAmount) * deltaTime;
    flake.Position.y += flake.Velocity.y * deltaTime;

    // Wrap around screen
    if (flake.Position.y > displaySize.y + 10.f)
    {
        flake.Position.y = -10.f;
        flake.Position.x = m_DistX(m_RandomGen);
        flake.Velocity.x = 0.f;
        flake.Velocity.y = m_DistSpeed(m_RandomGen);
    }

    if (flake.Position.x < -10.f)
        flake.Position.x = displaySize.x + 10.f;
    else if (flake.Position.x > displaySize.x + 10.f)
        flake.Position.x = -10.f;
}

void CSnow::Update(const ImVec2& displaySize)
{
    if (m_vSnowflakes.empty())
        return;

    float currentTime = ImGui::GetTime();
    float deltaTime = currentTime - m_flLastTime;
    m_flLastTime = currentTime;

    // Clamp delta time to prevent huge jumps
    if (deltaTime > 0.1f)
        deltaTime = 0.1f;

    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 mouseDelta;
    mouseDelta.x = mousePos.x - m_vLastMousePos.x;
    mouseDelta.y = mousePos.y - m_vLastMousePos.y;
    m_vLastMousePos = mousePos;

    for (auto& flake : m_vSnowflakes)
    {
        UpdateSnowflake(flake, deltaTime, displaySize, mousePos, mouseDelta);
    }
}

void CSnow::Render()
{
    if (m_vSnowflakes.empty())
        return;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    for (const auto& flake : m_vSnowflakes)
    {
        ImU32 color = IM_COL32(255, 255, 255, static_cast<int>(flake.Opacity * 255));

        // Draw snowflake as a small circle
        drawList->AddCircleFilled(
            flake.Position,
            flake.Size,
            color,
            8
        );

        // Optional: Add a subtle glow effect for larger snowflakes
        if (flake.Size > 3.5f)
        {
            ImU32 glowColor = IM_COL32(255, 255, 255, static_cast<int>(flake.Opacity * 50));
            drawList->AddCircleFilled(
                flake.Position,
                flake.Size * 1.5f,
                glowColor,
                8
            );
        }
    }
}

void CSnow::Clear()
{
    m_vSnowflakes.clear();
    m_bInitialized = false;
}