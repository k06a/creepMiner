// ==========================================================================
// 
// creepMiner - Burstcoin cryptocurrency CPU and GPU miner
// Copyright (C)  2016-2018 Creepsky (creepsky@gmail.com)
// 
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110 - 1301  USA
// 
// ==========================================================================

#include "Declarations.hpp"
#include <string>
#include <Poco/String.h>
#include <Poco/StringTokenizer.h>
#include <Poco/NumberParser.h>
#include "logging/MinerLogger.hpp"
#include "webserver/RequestHandler.hpp"

std::string Burst::Settings::cpuInstructionSet;
Burst::ProjectData Burst::Settings::project("creepMiner",
	Burst::Version(VERSION_MAJOR, VERSION_MINOR, VERSION_BUILD, VERSION_REVISION));

#ifdef USE_SSE4
const bool Burst::Settings::sse4 = true;
#else
const bool Burst::Settings::sse4 = false;
#endif

#ifdef USE_AVX
const bool Burst::Settings::avx = true;
#else
const bool Burst::Settings::avx = false;
#endif

#ifdef USE_AVX2
const bool Burst::Settings::avx2 = true;
#else
const bool Burst::Settings::avx2 = false;
#endif

#ifdef USE_NEON
const bool Burst::Settings::neon = true;
#else
const bool Burst::Settings::neon = false;
#endif

#ifdef USE_CUDA
const bool Burst::Settings::cuda = true;
#else
const bool Burst::Settings::cuda = false;
#endif

#ifdef USE_OPENCL
const bool Burst::Settings::openCl = true;
#else
const bool Burst::Settings::openCl = false;
#endif

void Burst::Version::updateLiterals()
{
	literal = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build);
	literalVerbose = literal + "." + std::to_string(revision);
}

Burst::Version::Version(uint32_t major, uint32_t minor, uint32_t build, uint32_t revision)
	: major{major}, minor{minor}, build{build}, revision{revision}
{
	updateLiterals();
}

Burst::Version::Version(std::string version)
{
	// trim left and right spaces
	Poco::trimInPlace(version);

	// remove all spaces inside
	Poco::replaceInPlace(version, " ", "");

	Poco::StringTokenizer tokenizer{ version, "." };

	// need major.minor.build (3 to 4 parts, concatenated by a dot)
	if (tokenizer.count() >= 3)
	{
		try
		{
			const auto& majorStr = tokenizer[0];
			const auto& minorStr = tokenizer[1];
			const auto& buildStr = tokenizer[2];

			major = Poco::NumberParser::parseUnsigned(majorStr);
			minor = Poco::NumberParser::parseUnsigned(minorStr);
			build = Poco::NumberParser::parseUnsigned(buildStr);

			if (tokenizer.count() == 4)
			{
				const auto& revisionStr = tokenizer[3];
				revision = Poco::NumberParser::parseUnsigned(revisionStr);
			}
			else
				revision = 0;

			updateLiterals();
		}
		catch (Poco::Exception& exc)
		{
			major = 0;
			minor = 0;
			build = 0;
			revision = 0;
			literal = "";

			log_error(MinerLogger::general, "Could not parse version from string! (%s)", version);
			log_exception(MinerLogger::general, exc);
		}
	}
}

bool Burst::Version::operator>(const Version& rhs) const
{
	// <this> vs <other>
	//
	// >2<.0.0 vs >1<.5.0
	if (major > rhs.major)
		return true;

	// >1<.5.0 vs >2<.0.0
	if (major < rhs.major)
		return false;

	// 1.>5<.0 vs 1.>4<.0
	if (minor > rhs.minor)
		return true;

	// 1.>4<.0 vs 1.>5<.0
	if (minor < rhs.minor)
		return false;

	// 1.5.>6< vs 1.5.>4<
	if (build > rhs.build)
		return true;

	// 1.5.>4< vs 1.5.>6<
	if (build < rhs.build)
		return false;

	// 1.5.6.>6< vs 1.5.6.>1<
	if (revision > rhs.revision)
		return true;

	// 1.5.6.>1< vs 1.5.6.>6<
	return false;
}

bool Burst::Version::operator==(const Version& rhs) const
{
	return major == rhs.major &&
		minor == rhs.minor &&
		build == rhs.build &&
		revision == rhs.revision;
}

bool Burst::Version::operator!=(const Version& rhs) const
{
	return !(*this == rhs);
}

void Burst::ProjectData::refreshNameAndVersion()
{
	Poco::Mutex::ScopedLock lock(mutex_);
	nameAndVersion = this->name + " " + this->version_.literal;
	nameAndVersionVerbose = this->name + " " +
		(this->version_.revision > 0 ? this->version_.literalVerbose : this->version_.literal) + " " +
		Settings::osFamily + " " + Settings::arch;

	if (!Settings::cpuInstructionSet.empty())
		nameAndVersionVerbose += std::string(" ") + Settings::cpuInstructionSet;
}

void Burst::ProjectData::refreshAndCheckOnlineVersion(Poco::Timer& timer)
{
	Poco::Mutex::ScopedLock lock(mutex_);
	const std::string host = "https://github.com/Creepsky/creepMiner";
	onlineVersion_ = Burst::RequestHandler::fetchOnlineVersion();
	if (onlineVersion_ > version_)
		log_system(MinerLogger::general, "There is a new version (%s) on\n\t%s", onlineVersion_.literal, host);
}

std::string Burst::ProjectData::getOnlineVersion() const
{
	Poco::Mutex::ScopedLock lock(mutex_);
	return onlineVersion_.literal;
}

const Burst::Version& Burst::ProjectData::getVersion() const
{
	Poco::Mutex::ScopedLock lock(mutex_);
	return version_;
}

Burst::ProjectData::ProjectData(std::string&& name, Version version)
	: name{std::move(name)}, version_{std::move(version)}, onlineVersion_{ std::move(version) }
{
	refreshNameAndVersion();
}

void Burst::Settings::setCpuInstructionSet(std::string cpuInstructionSet)
{
	cpuInstructionSet = std::move(cpuInstructionSet);
	project.refreshNameAndVersion();
}
