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

#include "Miner.hpp"
#include "logging/MinerLogger.hpp"
#include "MinerConfig.hpp"
#include "plots/PlotReader.hpp"
#include "MinerUtil.hpp"
#include "network/Request.hpp"
#include <Poco/Net/HTTPRequest.h>
#include "network/NonceSubmitter.hpp"
#include <Poco/JSON/Parser.h>
#include "plots/PlotSizes.hpp"
#include <Poco/FileStream.h>
#include <fstream>
#include <Poco/File.h>
#include <Poco/Delegate.h>
#include "plots/PlotVerifier.hpp"

namespace Burst
{
	namespace MinerHelper
	{
		template <typename T>
		void createWorkerDefault(std::unique_ptr<Poco::ThreadPool>& thread_pool, std::unique_ptr<Poco::TaskManager>& task_manager,
			const size_t size, Miner& miner, Poco::NotificationQueue& queue)
		{
			thread_pool = std::make_unique<Poco::ThreadPool>(1, static_cast<int>(size));
			task_manager = std::make_unique<Poco::TaskManager>(*thread_pool);

			auto submitFunction = [&miner](Poco::UInt64 nonce, Poco::UInt64 accountId, Poco::UInt64 deadline,
			                               Poco::UInt64 blockheight, const std::string& plotFile,
			                               bool ownAccount)
			{
				Deadline deadlineToAdd{nonce, deadline, miner.getAccount(accountId, ownAccount), blockheight, plotFile};
				deadlineToAdd.setWorker(MinerConfig::getConfig().getWorkerName());
				NonceConfirmation _;
				const auto addedDeadline = miner.addDeadline(std::move(deadlineToAdd), _);
				if (addedDeadline != nullptr)
				{
					std::thread thread{[=, &miner]()
					{
						miner.submitDeadline(addedDeadline);
					}};

					thread.detach();
				}
			};

			for (size_t i = 0; i < size; ++i)
				task_manager->start(new T(miner.getData(), queue, submitFunction));
		}

		template <typename T, typename ...Args>
		void createWorker(std::unique_ptr<Poco::ThreadPool>& thread_pool, std::unique_ptr<Poco::TaskManager>& task_manager,
			const size_t size, Args&&... args)
		{
			thread_pool = std::make_unique<Poco::ThreadPool>(1, static_cast<int>(size));
			task_manager = std::make_unique<Poco::TaskManager>(*thread_pool);

			for (size_t i = 0; i < size; ++i)
				task_manager->start(new T(std::forward<Args>(args)...));
		}
	}
}

Burst::Miner::Miner()
{}

Burst::Miner::~Miner() = default;

void Burst::Miner::run()
{
	poco_ndc(Miner::run);
	running_ = true;
	progressRead_ = std::make_shared<PlotReadProgress>();
	progressVerify_ = std::make_shared<PlotReadProgress>();

	progressRead_->progressChanged.add(Poco::delegate(this, &Miner::progressChanged));
	progressVerify_->progressChanged.add(Poco::delegate(this, &Miner::progressChanged));

	auto& config = MinerConfig::getConfig();
	auto errors = 0u;

	if (config.getPoolUrl().empty() &&
		config.getMiningInfoUrl().empty() &&
		config.getWalletUrl().empty())
	{
		log_critical(MinerLogger::miner, "One of the URLs are all empty! Therfore the miner is shutting down!");
		return;
	}

	Poco::ThreadPool::defaultPool().addCapacity(128);

	MinerConfig::getConfig().printConsole();

	// only create the thread pools and manager for mining if there is work to do (plot files)
	if (!config.getPlotFiles().empty())
	{
		// manager
		nonceSubmitterManager_ = std::make_unique<Poco::TaskManager>();

		// create the plot readers
		MinerHelper::createWorker<PlotReader>(plotReaderPool_, plotReader_, MinerConfig::getConfig().getMaxPlotReaders(),
			data_, progressRead_, progressVerify_, verificationQueue_, plotReadQueue_);

		// create the plot verifiers
		createPlotVerifiers();

#ifndef USE_CUDA
		if (config.getProcessorType() == "CUDA")
			log_error(MinerLogger::miner, "You are mining with your CUDA GPU, but the miner is compiled without the CUDA SDK!\n"
				"You will not see any deadline coming from this miner!");
#endif
	}

	wallet_ = MinerConfig::getConfig().getWalletUrl();

	const auto wakeUpTime = static_cast<long>(config.getWakeUpTime());

	if (wakeUpTime > 0)
	{
		const Poco::TimerCallback<Miner> callback(*this, &Miner::onWakeUp);

		wakeUpTimer_.setPeriodicInterval(wakeUpTime * 1000);
		wakeUpTimer_.start(callback);
	}
	
	running_ = true;

	miningInfoSession_ = MinerConfig::getConfig().createSession(HostType::MiningInfo);
	miningInfoSession_->setKeepAlive(true);

	// TODO REWORK
	//wallet_.getLastBlock(currentBlockHeight_);

	log_information(MinerLogger::miner, "Fetching getMiningInfo from pool");
	size_t currentMiningInfoIndex = 0;
	std::string currentUrl;

	while (running_)
	{
		try
		{
			const auto& altMiningInfoUrls = MinerConfig::getConfig().getMiningInfoUrlAlt();
			
			std::vector<Url> miningInfoUrls;
			miningInfoUrls.reserve(altMiningInfoUrls.size() + 1);
			miningInfoUrls.emplace_back(MinerConfig::getConfig().getMiningInfoUrl());
			miningInfoUrls.insert(miningInfoUrls.end(), altMiningInfoUrls.begin(), altMiningInfoUrls.end());

			if (currentMiningInfoIndex >= miningInfoUrls.size())
				currentMiningInfoIndex = 0;

			const auto& miningInfoUrl = miningInfoUrls[currentMiningInfoIndex];

			if (miningInfoUrl.getCanonical(true) != currentUrl)
			{
				currentUrl = miningInfoUrl.getCanonical(true);
				log_debug(MinerLogger::miner, "Switched getMiningInfo source to %s", currentUrl);
			}
			
			if (getMiningInfo(miningInfoUrl))
				errors = 0;
			else
			{
				++errors;
				log_debug(MinerLogger::miner, "Could not fetch getMiningInfo %s (%u/5 times total)...",
					miningInfoUrl.getCanonical(), errors);
				++currentMiningInfoIndex;
				miningInfoSession_.reset();
			}

			// we have a tollerance of 5 times of not being able to fetch mining infos, before its a real error
			if (errors >= 5)
			{
				// reset error-counter and show error-message in console
				log_error(MinerLogger::miner, "Could not fetch getMiningInfo!");
				errors = 0;
			}
		}
		catch (const Poco::Exception& e)
		{
			log_error(MinerLogger::miner, "Could not fetch getMiningInfo: %s", e.displayText());
			log_current_stackframe(MinerLogger::miner);
		}

		std::this_thread::sleep_for(std::chrono::seconds(MinerConfig::getConfig().getMiningInfoInterval()));
	}

	if (wakeUpTime > 0)
		wakeUpTimer_.stop();

	running_ = false;
}

void Burst::Miner::stop()
{
	poco_ndc(Miner::stop);

	// stop plot reader
	if (plotReader_ != nullptr)
		shutDownWorker(*plotReaderPool_, *plotReader_, plotReadQueue_);

	// stop verifier
	if (verifier_ != nullptr)
		shutDownWorker(*verifierPool_, *verifier_, verificationQueue_);
	
	running_ = false;
}

void Burst::Miner::restart()
{
	restart_ = true;
	stop();
}

void Burst::Miner::addPlotReadNotifications(bool wakeUpCall)
{
	const auto initPlotReadNotification = [this, wakeUpCall](PlotDir& plotDir)
	{
		auto notification = new PlotReadNotification;
		notification->dir = plotDir.getPath();
		notification->gensig = getGensig();
		notification->scoopNum = getScoopNum();
		notification->blockheight = getBlockheight();
		notification->baseTarget = getBaseTarget();
		notification->type = plotDir.getType();
		notification->wakeUpCall = wakeUpCall;

		for (const auto& plotFile : plotDir.getPlotfiles(true))
			accounts_.getAccount(plotFile->getAccountId(), wallet_, true);

		return notification;
	};

	const auto addParallel = [this, &initPlotReadNotification](PlotDir& plotDir, std::shared_ptr<PlotFile> plotFile)
	{
		auto plotRead = initPlotReadNotification(plotDir);
		plotRead->plotList.emplace_back(plotFile);
		plotReadQueue_.enqueueNotification(plotRead);
	};

	MinerConfig::getConfig().forPlotDirs([this, &addParallel, &initPlotReadNotification](PlotDir& plotDir)
	{
		if (plotDir.getType() == PlotDir::Type::Parallel)
		{
			for (const auto& plotFile : plotDir.getPlotfiles())
				addParallel(plotDir, plotFile);

			for (const auto& relatedPlotDir : plotDir.getRelatedDirs())
				for (const auto& plotFile : relatedPlotDir->getPlotfiles())
					addParallel(plotDir, plotFile);
		}
		else
		{
			auto plotRead = initPlotReadNotification(plotDir);
			plotRead->plotList = plotDir.getPlotfiles();

			for (const auto& relatedPlotDir : plotDir.getRelatedDirs())
				plotRead->relatedPlotLists.emplace_back(relatedPlotDir->getPath(), relatedPlotDir->getPlotfiles());

			plotReadQueue_.enqueueNotification(plotRead);
		}

		return true;
	});
}

bool Burst::Miner::wantRestart() const
{
	return restart_;
}

void Burst::Miner::updateGensig(const std::string& gensigStr, Poco::UInt64 blockHeight, Poco::UInt64 baseTarget)
{
	poco_ndc(Miner::updateGensig);

	try
	{
		// stop all reading processes if any
		if (!MinerConfig::getConfig().getPlotFiles().empty())
		{
			log_debug(MinerLogger::miner, "Plot-read-queue: %d (%d reader), verification-queue: %d (%d verifier)",
				plotReadQueue_.size(), plotReader_->count(), verificationQueue_.size(), verifier_->count());
			log_debug(MinerLogger::miner, "Allocated memory: %s", memToString(PlotReader::globalBufferSize.getSize(), 1));
		
			PlotReader::globalBufferSize.setMax(MinerConfig::getConfig().getMaxBufferSize());
		}
		
		// clear the plot read queue
		plotReadQueue_.clear();

		// Set dynamic targetDL for this round if a submitProbability is given
		if (MinerConfig::getConfig().getSubmitProbability() > 0.)
		{
			const auto difficultyFl = 18325193796.0 / static_cast<float>(baseTarget);
			const auto tarDlFac = MinerConfig::getConfig().getTargetDLFactor();
			const auto totAccPlotsize = static_cast<double>(PlotSizes::getTotalBytes(PlotSizes::Type::Combined)) / 1024.f / 1024.f / 1024.f / 1024.f;

			auto blockTargetDeadline = MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Local);
			if (totAccPlotsize > 0 && MinerConfig::getConfig().getSubmitProbability() > 0.)
				blockTargetDeadline = static_cast<Poco::UInt64>(tarDlFac * difficultyFl / totAccPlotsize);
			const auto poolDeadline = MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Pool);

			if (blockTargetDeadline < poolDeadline || poolDeadline == 0)
				MinerConfig::getConfig().setTargetDeadline(blockTargetDeadline, TargetDeadlineType::Local);
			else
				MinerConfig::getConfig().setTargetDeadline(poolDeadline, TargetDeadlineType::Local);
		}

		// Set total run time of previous block
		if (startPoint_.time_since_epoch().count() > 0)
		{
			const auto timeDiff = std::chrono::high_resolution_clock::now() - startPoint_;
			const auto timeDiffSeconds = std::chrono::duration_cast<std::chrono::seconds>(timeDiff);
			log_unimportant(MinerLogger::miner, "Block %s ended in %s", numberToString(blockHeight - 1),
				deadlineFormat(timeDiffSeconds.count()));
			data_.getBlockData()->setBlockTime(timeDiffSeconds.count());
		}

		// setup new block-data
		auto block = data_.startNewBlock(blockHeight, baseTarget, gensigStr, MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Local));
		setIsProcessing(true);

		// printing block info and transfer it to local server
		{
			const auto difficulty = block->getDifficulty();
			const auto difficultyDifference = data_.getDifficultyDifference();
			std::string diffiultyDifferenceToString;

			if (difficultyDifference < 0)
				diffiultyDifferenceToString = numberToString(difficultyDifference);
			else if (difficultyDifference == 0)
				diffiultyDifferenceToString = "no change";
			else
				diffiultyDifferenceToString = '+' + numberToString(difficultyDifference);

			log_notice(MinerLogger::miner, std::string(50, '-') + "\n"
				"block#     \t%s\n"
				"scoop#     \t%Lu\n"
				"baseTarget#\t%s\n"
				"gensig     \t%s\n"
				"difficulty \t%s (%s)\n"
				"targetDL   \t%s\n" +
				std::string(50, '-'),
				numberToString(blockHeight), block->getScoop(), numberToString(baseTarget), createTruncatedString(getGensigStr(), 14, 32),
				numberToString(difficulty),
				diffiultyDifferenceToString,
				deadlineFormat(MinerConfig::getConfig().getTargetDeadline())
			);

			data_.getBlockData()->refreshBlockEntry();
		}

		if (MinerConfig::getConfig().isRescanningEveryBlock())
			MinerConfig::getConfig().rescanPlotfiles();

		progressRead_->reset(blockHeight, MinerConfig::getConfig().getTotalPlotsize());
		progressVerify_->reset(blockHeight, MinerConfig::getConfig().getTotalPlotsize());

		PlotSizes::nextRound();
		PlotSizes::refresh(Poco::Net::IPAddress{"127.0.0.1"});
		startPoint_ = std::chrono::high_resolution_clock::now();

		addPlotReadNotifications();

		data_.getWonBlocksAsync(wallet_, accounts_);

		// why we start a new thread to gather the last winner:
		// it could be slow and is not necessary for the whole process
		// so show it when it's done
		if (blockHeight > 0 && wallet_.isActive())
			block->getLastWinnerAsync(wallet_, accounts_);
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not update the gensig: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

std::shared_ptr<Burst::Deadline> Burst::Miner::addDeadline(Deadline deadline, NonceConfirmation& confirmation)
{
	poco_ndc(Miner::addNewDeadline);

	try
	{
		const auto currentHeight = getBlockheight();

		if (deadline.getBlock() != currentHeight)
		{
			log_debug(MinerLogger::miner, deadline.toActionString("nonce discarded - wrong block"));
			confirmation = NonceConfirmation::createWrongBlock(deadline.getBlock(), currentHeight, deadline.getNonce(),
			                                                   deadline.getDeadline());
		}
		else
		{
			const auto targetDeadline = MinerConfig::getConfig().getTargetDeadline();
			const auto tooHigh = targetDeadline > 0 && deadline.getDeadline() > targetDeadline;

			auto block = data_.getBlockData();

			if (block == nullptr)
			{
				log_debug(MinerLogger::miner, deadline.toActionString("nonce discarded - no block data"));
				confirmation = NonceConfirmation::createError(deadline.getNonce(), deadline.getDeadline(), "No block data");
			}
			else
			{
				auto addedDeadline = block->addDeadlineIfBest(std::move(deadline));

				if (addedDeadline != nullptr)
				{
					addedDeadline->found(tooHigh);

					auto output = MinerLogger::hasOutput(NonceFound) || (tooHigh && MinerLogger::hasOutput(NonceFoundTooHigh));

					if (tooHigh)
						output = MinerLogger::hasOutput(NonceFoundTooHigh);

					log_unimportant_if(MinerLogger::miner, output, addedDeadline->toActionString("nonce found"));
						
					if (tooHigh)
					{
						log_debug(MinerLogger::miner, addedDeadline->toActionString("nonce discarded - deadline too high"));
						confirmation = NonceConfirmation::createTooHigh(addedDeadline->getNonce(), addedDeadline->getDeadline(), targetDeadline);
					}
					else
					{
						return addedDeadline;
					}
				}
			}
			//else
			//{
			//	log_debug(MinerLogger::miner, deadline->toActionString("nonce discarded - not best"));
			//	const auto best = block->getBestDeadline();
			//	Poco::UInt64 bestDeadline = 0;
			//	if (best != nullptr)
			//		bestDeadline = best->getDeadline();
			//	nonceConfirmation = NonceConfirmation::createNotBest(deadline->getNonce(), deadline->getDeadline(), bestDeadline);
			//}
		}
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, deadline.toActionString("nonce discarded - error occured") + Poco::format("\n\tReason: %s", e.displayText()));
		log_current_stackframe(MinerLogger::miner);
		confirmation = NonceConfirmation::createError(deadline.getNonce(), deadline.getDeadline(), e.displayText());
	}

	return nullptr;
}

Burst::NonceConfirmation Burst::Miner::submitDeadline(std::shared_ptr<Deadline> deadline)
{
	return NonceSubmitter{*this, deadline}.submit();
}

const Burst::GensigData& Burst::Miner::getGensig() const
{
	const auto blockData = data_.getBlockData();

	if (blockData == nullptr)
	{
		log_current_stackframe(MinerLogger::miner);
		throw Poco::NullPointerException{"Tried to access the gensig from a nullptr!"};
	}

	return blockData->getGensig();
}

const std::string& Burst::Miner::getGensigStr() const
{
	const auto blockData = data_.getBlockData();

	if (blockData == nullptr)
	{
		log_current_stackframe(MinerLogger::miner);
		throw Poco::NullPointerException{"Tried to access the gensig from a nullptr!"};
	}

	return blockData->getGensigStr();
}

Poco::UInt64 Burst::Miner::getBaseTarget() const
{
	return data_.getCurrentBasetarget();
}

Poco::UInt64 Burst::Miner::getBlockheight() const
{
	return data_.getCurrentBlockheight();
}

bool Burst::Miner::hasBlockData() const
{
	return data_.getBlockData() != nullptr;
}

Poco::UInt64 Burst::Miner::getScoopNum() const
{
	return data_.getCurrentScoopNum();
}

bool Burst::Miner::isProcessing() const
{
	return isProcessing_;
}

bool Burst::Miner::getMiningInfo(const Url& url)
{
	using namespace Poco::Net;
	poco_ndc(Miner::getMiningInfo);

	try
	{
		if (miningInfoSession_ == nullptr && !url.empty())
			miningInfoSession_ = MinerConfig::getConfig().createSession(url);

		Request request(std::move(miningInfoSession_));

		HTTPRequest requestData { HTTPRequest::HTTP_GET, "/burst?requestType=getMiningInfo", HTTPRequest::HTTP_1_1 };
		requestData.setKeepAlive(true);

		auto response = request.send(requestData);
		std::string responseData;
		
		if (response.receive(responseData))
		{
			try
			{
				if (responseData.empty())
					return false;

				HttpResponse httpResponse(responseData);
				Poco::JSON::Parser parser;
				Poco::JSON::Object::Ptr root;

				try
				{
					root = parser.parse(httpResponse.getMessage()).extract<Poco::JSON::Object::Ptr>();
				}
				catch (...)
				{
					return false;
				}

				std::string gensig;

				if (root->has("height"))
				{
					const std::string newBlockHeightStr = root->get("height");
					const auto newBlockHeight = std::stoull(newBlockHeightStr);

					if (data_.getBlockData() == nullptr ||
						newBlockHeight > data_.getBlockData()->getBlockheight())
					{
						std::string baseTargetStr;

						if (root->has("baseTarget"))
							baseTargetStr = root->get("baseTarget").convert<std::string>();

						if (root->has("generationSignature"))
							gensig = root->get("generationSignature").convert<std::string>();

						if (root->has("targetDeadline"))
						{
							// remember the current pool target deadline
							const auto targetDeadlinePoolBefore = MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Pool);

							// get the target deadline from pool
							auto targetDeadlinePoolJson = root->get("targetDeadline");
							Poco::UInt64 targetDeadlinePool = 0;
							
							// update the new pool target deadline
							if (!targetDeadlinePoolJson.isEmpty())
								targetDeadlinePool = targetDeadlinePoolJson.convert<Poco::UInt64>();

							MinerConfig::getConfig().setTargetDeadline(targetDeadlinePool, TargetDeadlineType::Pool);

							// if its changed, print it
							if (MinerConfig::getConfig().getSubmitProbability() == 0.)
							{
								if (targetDeadlinePoolBefore != MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Pool))
									log_system(MinerLogger::config, std::string(50, '-') + "\n" +
										"got new target deadline from pool\n"
										"old pool target deadline:    \t%s\n"
										"new pool target deadline:    \t%s\n"
										"target deadline from config: \t%s\n"
										"lowest target deadline:      \t%s\n" +
										std::string(50, '-'),
										deadlineFormat(targetDeadlinePoolBefore),
										deadlineFormat(MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Pool)),
										deadlineFormat(MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Local)),
										deadlineFormat(MinerConfig::getConfig().getTargetDeadline()));
							}
							else
							{
								if (targetDeadlinePoolBefore != MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Pool))
									log_system(MinerLogger::config, std::string(50, '-') + "\n" +
										"got new target deadline from pool\n"
										"old pool target deadline:    \t%s\n"
										"new pool target deadline:    \t%s\n" +
										std::string(50, '-'),
										deadlineFormat(targetDeadlinePoolBefore),
										deadlineFormat(MinerConfig::getConfig().getTargetDeadline(TargetDeadlineType::Pool)));
							}
						}

						updateGensig(gensig, newBlockHeight, std::stoull(baseTargetStr));
					}
				}

				transferSession(response, miningInfoSession_);
				return true;
			}
			catch (Poco::Exception& exc)
			{
				log_error(MinerLogger::miner, "Error on getting new block information!\n\t%s", exc.displayText());
				// because the full response may be too long, we only log the it in the logfile
				log_file_only(MinerLogger::miner, Poco::Message::PRIO_ERROR, TextType::Error, "Block information full response:\n%s", responseData);
				log_current_stackframe(MinerLogger::miner);
			}
		}

		transferSession(request, miningInfoSession_);
		transferSession(response, miningInfoSession_);

		return false;
	}
	catch (const Poco::NotFoundException& e)
	{
		log_error(MinerLogger::miner, "Could not get the mining information: %s", e.displayText());
		return false;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get the mining information: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return false;
	}
}

void Burst::Miner::shutDownWorker(Poco::ThreadPool& threadPool, Poco::TaskManager& taskManager, Poco::NotificationQueue& queue) const
{
	Poco::Mutex::ScopedLock lock(workerMutex_);
	queue.wakeUpAll();
	taskManager.cancelAll();
	threadPool.stopAll();
	threadPool.joinAll();
}

namespace Burst
{
	std::mutex progressMutex;
	Progress progress;

	void showProgress(PlotReadProgress& progressRead, PlotReadProgress& progressVerify, MinerData& data, Poco::UInt64 blockheight,
		std::chrono::high_resolution_clock::time_point& startPoint,
		const std::function<void(Poco::UInt64, double)>& blockProcessed)
	{
		std::lock_guard<std::mutex> lock(progressMutex);

		const auto readProgressPercent = progressRead.getProgress();
		const auto verifyProgressPercent = progressVerify.getProgress();
		const auto readProgressValue = progressRead.getValue();
		const auto verifyProgressValue = progressVerify.getValue();

		const auto timeDiff = std::chrono::high_resolution_clock::now() - startPoint;
		const auto timeDiffSeconds = std::chrono::duration<double>(timeDiff);

		const auto readProgressChanged = progress.read != readProgressPercent;
		const auto verifyProgressChanged = progress.verify != readProgressPercent;

		if (readProgressChanged || verifyProgressChanged)
		{
			progress.read = readProgressPercent;
			progress.verify = verifyProgressPercent;
			progress.bytesPerSecondRead = readProgressValue / 4096. / timeDiffSeconds.count();
			progress.bytesPerSecondVerify = verifyProgressValue / 4096. / timeDiffSeconds.count();
			progress.bytesPerSecondCombined = (readProgressValue + verifyProgressValue) / 4096. / timeDiffSeconds.
				count();
			
			MinerLogger::writeProgress(progress);
			data.getBlockData()->setProgress(readProgressPercent, verifyProgressPercent, blockheight);
			
			if (readProgressPercent == 100.f && verifyProgressPercent == 100.f && blockProcessed != nullptr)
			{
				blockProcessed(blockheight, timeDiffSeconds.count());
				progress.read = 0.f;
				progress.verify = 0.f;
			}
		}
	}
}

void Burst::Miner::progressChanged(float &progress)
{
	showProgress(*progressRead_, *progressVerify_, getData(), getBlockheight(), startPoint_,
	             [this](Poco::UInt64 blockHeight, double roundTime) { onRoundProcessed(blockHeight, roundTime); });
}

void Burst::Miner::onWakeUp(Poco::Timer& timer)
{
	addPlotReadNotifications(true);
}

void Burst::Miner::onRoundProcessed(Poco::UInt64 blockHeight, double roundTime)
{
	poco_ndc(Miner::onRoundProcessed);

	try
	{
		const auto block = data_.getBlockData();
		setIsProcessing(false);

		if (block == nullptr || block->getBlockheight() != blockHeight)
			return;

		block->setRoundTime(roundTime);
		const auto bestDeadline = block->getBestDeadline(BlockData::DeadlineSearchType::Found);

		log_information(MinerLogger::miner, std::string(50, '-') + "\n"
			"processed block \t%s\n"
			"round time      \t%ss\n"
			"best deadline   \t%s\n" +
			std::string(50, '-'),
			numberToString(block->getBlockheight()),
			Poco::NumberFormatter::format(roundTime, 3),
			bestDeadline == nullptr ? "none" : deadlineFormat(bestDeadline->getDeadline()));
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not processes the round end: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

std::shared_ptr<Burst::Deadline> Burst::Miner::getBestSent(Poco::UInt64 accountId, Poco::UInt64 blockHeight) const
{
	poco_ndc(Miner::getBestSent);

	const auto block = data_.getBlockData();

	if (block == nullptr ||
		blockHeight != block->getBlockheight())
		return nullptr;

	return block->getBestDeadline(accountId, BlockData::DeadlineSearchType::Sent);
}

std::shared_ptr<Burst::Deadline> Burst::Miner::getBestConfirmed(Poco::UInt64 accountId, Poco::UInt64 blockHeight) const
{
	poco_ndc(Miner::getBestConfirmed);

	const auto block = data_.getBlockData();

	if (block == nullptr ||
		blockHeight != block->getBlockheight())
		return nullptr;

	return block->getBestDeadline(accountId, BlockData::DeadlineSearchType::Confirmed);
}

Burst::MinerData& Burst::Miner::getData()
{
	return data_;
}

std::shared_ptr<Burst::Account> Burst::Miner::getAccount(AccountId id, bool persistent)
{
	return accounts_.getAccount(id, wallet_, persistent);
}

void Burst::Miner::createPlotVerifiers()
{
	const auto& processorType = MinerConfig::getConfig().getProcessorType();
	auto cpuInstructionSet = MinerConfig::getConfig().getCpuInstructionSet();
	auto forceCpu = false, fallback = false;
	const auto createWorker = [this](std::function<void(std::unique_ptr<Poco::ThreadPool>&,
	                                                    std::unique_ptr<Poco::TaskManager>&,
	                                                    size_t, Miner&, Poco::NotificationQueue&)> function)
	{
		function(verifierPool_, verifier_, MinerConfig::getConfig().getMiningIntensity(), *this, verificationQueue_);
	};

	if (processorType == "CUDA")
	{
		if (Settings::cuda)
			createWorker(MinerHelper::createWorkerDefault<PlotVerifierCuda>);
		else
			forceCpu = true;
	}
	else if (processorType == "OPENCL")
	{
		if (Settings::openCl)
			createWorker(MinerHelper::createWorkerDefault<PlotVerifierOpencl>);
		else
			forceCpu = true;
	}

	if (processorType == "CPU" || forceCpu)
	{
		if (cpuInstructionSet == "SSE4" && Settings::sse4)
			createWorker(MinerHelper::createWorkerDefault<PlotVerifierSse4>);
		else if (cpuInstructionSet == "AVX" && Settings::avx)
			createWorker(MinerHelper::createWorkerDefault<PlotVerifierAvx>);
		else if (cpuInstructionSet == "AVX2" && Settings::avx2)
			createWorker(MinerHelper::createWorkerDefault<PlotVerifierAvx2>);
		else if (cpuInstructionSet == "NEON" && Settings::neon)
			createWorker(MinerHelper::create_worker_default<PlotVerifierNeon>);
		else if (cpuInstructionSet == "SSE2")
			createWorker(MinerHelper::createWorkerDefault<PlotVerifierSse2>);
		else
			fallback = true;
	}

	if (fallback)
		cpuInstructionSet = "SSE2";

	if (forceCpu)
	{
		log_warning(MinerLogger::miner, "You are using the processor type %s with the instruction set %s,\n"
			"but your miner is compiled without that feature!\n"
			"As a fallback solution your CPU with the instruction set %s is used.", processorType, MinerConfig::getConfig().
			getCpuInstructionSet(), cpuInstructionSet);
		
		createWorker(MinerHelper::createWorkerDefault<PlotVerifierSse2>);
	}
}

void Burst::Miner::setMiningIntensity(unsigned intensity)
{
	Poco::Mutex::ScopedLock lock(workerMutex_);

	// dont change it if its the same intensity
	// changing it is a heavy task
	if (MinerConfig::getConfig().getMiningIntensity() == intensity)
		return;

	shutDownWorker(*verifierPool_, *verifier_, verificationQueue_);
	MinerConfig::getConfig().setMininigIntensity(intensity);
	createPlotVerifiers();
}

void Burst::Miner::setMaxPlotReader(unsigned max_reader)
{
	Poco::Mutex::ScopedLock lock(workerMutex_);

	// dont change it if its the same reader count
	// changing it is a heavy task
	if (MinerConfig::getConfig().getMaxPlotReaders() == max_reader)
		return;

	shutDownWorker(*plotReaderPool_, *plotReader_, plotReadQueue_);
	MinerConfig::getConfig().setMaxPlotReaders(max_reader);
	MinerHelper::createWorker<PlotReader>(plotReaderPool_, plotReader_, MinerConfig::getConfig().getMaxPlotReaders(),
		data_, progressRead_, progressVerify_, verificationQueue_, plotReadQueue_);
}

void Burst::Miner::setMaxBufferSize(Poco::UInt64 size)
{
	MinerConfig::getConfig().setBufferSize(size);
	PlotReader::globalBufferSize.setMax(size);
}

void Burst::Miner::rescanPlotfiles()
{
	// rescan the plot files...
	if (MinerConfig::getConfig().rescanPlotfiles())
	{
		// we send the new settings (size could be changed)
		data_.getBlockData()->refreshConfig();

		// then we send all plot dirs and files
		data_.getBlockData()->refreshPlotDirs();
	}
	else
	{
		log_success(MinerLogger::config, "No plot dir changes");
	}
}

void Burst::Miner::setIsProcessing(bool isProc)
{
	Poco::Mutex::ScopedLock lock(workerMutex_);

	isProcessing_ = isProc;
}

bool Burst::Miner::isPoC2() const
{
	const auto block = data_.getBlockData();
	poco_check_ptr(block);
	return block->getBlockheight() >= MinerConfig::getConfig().getPoc2StartBlock();
}
