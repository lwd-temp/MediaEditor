#include <sstream>
#include "imgui_helper.h"
#include "FileSystemUtils.h"
#include "MecProject.h"

using namespace std;
using namespace Logger;

namespace MEC
{
string Project::GetDefaultProjectBaseDir()
{
    auto userVideoDir = ImGuiHelper::getVideoFolder();
    return SysUtils::JoinPath(userVideoDir, "MecProject");
}

Project::ErrorCode Project::CreateNew(const string& name, const string& baseDir)
{
    lock_guard<recursive_mutex> _lk(m_mtxApiLock);
    if (m_bOpened)
    {
        const auto errcode = Save();
        if (errcode != OK)
        {
            m_pLogger->Log(Error) << "FAILED to save current project '" << m_projName << "' before creating new project!" << endl;
            return errcode;
        }
    }
    auto projDir = SysUtils::JoinPath(baseDir, name);
    if (SysUtils::Exists(projDir))
    {
        m_pLogger->Log(Error) << "Project directory path '" << projDir << "' already exists! Can NOT create new project at this location." << endl;
        return ALREADY_EXISTS;
    }
    if (!SysUtils::CreateDirectory(projDir, true))
    {
        m_pLogger->Log(Error) << "FAILED to create project directory at '" << projDir << "'!" << endl;
        return MKDIR_FAILED;
    }
    m_projName = name;
    m_projDir = projDir;
    m_projFilePath = SysUtils::JoinPath(m_projDir, m_projName+".mep");
    m_projVer = (VER_MAJOR<<24) | (VER_MINOR<<16);
    m_bOpened = true;
    return OK;
}

Project::ErrorCode Project::Load(const std::string& projFilePath)
{
    lock_guard<recursive_mutex> _lk(m_mtxApiLock);
    if (m_bOpened)
    {
        const auto errcode = Save();
        if (errcode != OK)
        {
            m_pLogger->Log(Error) << "FAILED to save current project '" << m_projName << "' before loading another project!" << endl;
            return errcode;
        }
    }
    if (!SysUtils::IsFile(projFilePath))
    {
        m_pLogger->Log(Error) << "FAILED to load project from '" << projFilePath << "'! Target is NOT a file." << endl;
        return FILE_INVALID;
    }
    auto res = imgui_json::value::load(projFilePath);
    if (!res.second)
    {
        m_pLogger->Log(Error) << "FAILED to parse project json from '" << projFilePath << "'!" << endl;
        return PARSE_FAILED;
    }
    const auto& jnProj = res.first;
    string attrName = "mec_proj_version";
    if (jnProj.contains(attrName) && jnProj[attrName].is_number())
    {
        m_projVer = (uint32_t)jnProj[attrName].get<imgui_json::number>();
        m_jnProjContent = std::move(jnProj["proj_content"]);
        m_projName = jnProj["proj_name"].get<imgui_json::string>();
        m_projDir = SysUtils::ExtractDirectoryPath(projFilePath);
    }
    else
    {
        m_jnProjContent = std::move(jnProj);
        m_projName = SysUtils::ExtractFileBaseName(projFilePath);
    }
    m_projFilePath = projFilePath;
    m_bOpened = true;
    return OK;
}

Project::ErrorCode Project::Save()
{
    lock_guard<recursive_mutex> _lk(m_mtxApiLock);
    if (!m_bOpened)
        return NOT_OPENED;
    if (!m_jnProjContent.is_object())
        return TL_INVALID;
    imgui_json::value jnProj;
    jnProj["mec_proj_version"] = imgui_json::number(m_projVer);
    jnProj["proj_name"] = imgui_json::string(m_projName);
    jnProj["proj_content"] = m_jnProjContent;
    if (!jnProj.save(m_projFilePath))
    {
        m_pLogger->Log(Error) << "FAILED to save project json file at '" << m_projFilePath << "'!" << endl;
        return FAILED;
    }
    return OK;
}

Project::ErrorCode Project::Close(bool bSaveBeforeClose)
{
    lock_guard<recursive_mutex> _lk(m_mtxApiLock);
    if (!m_bOpened)
        return OK;
    if (bSaveBeforeClose)
    {
        const auto errcode = Save();
        if (errcode != OK)
        {
            m_pLogger->Log(Error) << "FAILED to save current project '" << m_projName << "' before closing the project!" << endl;
            return errcode;
        }
    }
    m_jnProjContent = nullptr;
    m_projDir.clear();
    m_projName.clear();
    m_projFilePath.clear();
    m_projVer = 0;
    m_bOpened = false;
    return OK;
}

// vector<BackgroundTask::Holder> Project::GetBackgroundTasks()
// {
//     lock_guard<recursive_mutex> _lk(m_mtxApiLock);
//     if (!m_bOpened)
//         return {};
//     vector<BackgroundTask::Holder> result(m_aBgtasks.size());
//     auto itSrc = m_aBgtasks.begin();
//     auto itDst = result.begin();
//     while (itSrc != m_aBgtasks.end())
//         *itDst++ = *itSrc++;
//     return std::move(result);
// }

uint8_t Project::VER_MAJOR = 1;
uint8_t Project::VER_MINOR = 0;

Project::Holder Project::CreateInstance()
{
    return Project::Holder(new Project());
}
}