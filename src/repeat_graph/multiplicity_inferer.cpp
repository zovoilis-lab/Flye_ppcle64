//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#include "multiplicity_inferer.h"
#include "graph_processing.h"
#include "../common/disjoint_set.h"
#include "../common/utils.h"
#include <cmath>

static const int MIN_JCT_SUPPORT = 2;


//Estimates the mean coverage and assingns edges multiplicity accordingly
void MultiplicityInferer::estimateCoverage()
{
	const int WINDOW = Config::get("coverage_estimate_window");

	//alternative coverage
	std::unordered_map<GraphEdge*, std::vector<int32_t>> wndCoverage;

	for (auto& edge : _graph.iterEdges())
	{
		size_t numWindows = edge->length() / WINDOW;
		wndCoverage[edge].assign(numWindows, 0);
	}

	for (auto& path : _aligner.getAlignments())
	{
		for (size_t i = 0; i < path.size(); ++i)
		{
			auto& ovlp = path[i].overlap;
			auto& coverage = wndCoverage[path[i].edge];
			for (int pos = ovlp.extBegin / WINDOW + 1; 
			 	 pos < ovlp.extEnd / WINDOW; ++pos)
			{
				if (pos >= 0 && 
					pos < (int)coverage.size())
				{
					++coverage[pos];
				}
			}
		}
	}

	int64_t sumCov = 0;
	int64_t sumLength = 0;
	for (auto& edgeCoverage : wndCoverage)
	{
		for (auto& cov : edgeCoverage.second)
		{
			sumCov += (int64_t)cov;
			++sumLength;
		}
	}
	_meanCoverage = (sumLength != 0) ? sumCov / sumLength : /*defaut*/ 1;

	Logger::get().info() << "Mean edge coverage: " << _meanCoverage;

	std::vector<int32_t> edgesCoverage;
	for (auto edge : _graph.iterEdges())
	{
		if (wndCoverage[edge].empty()) continue;

		GraphEdge* complEdge = _graph.complementEdge(edge);
		int32_t medianCov = (median(wndCoverage[edge]) + 
						 	 median(wndCoverage[complEdge])) / 2;

		int estMult = std::round((float)medianCov / _meanCoverage);
		if (estMult == 1)
		{
			edgesCoverage.push_back(medianCov);
		}

		//std::string match = estMult != edge->multiplicity ? "*" : " ";
		std::string covStr;

		Logger::get().debug() << edge->edgeId.signedId() << "\tlen:"
				<< edge->length() << "\tcov:" << medianCov << "\tmult:"
				<< (float)medianCov / _meanCoverage;

		//edge->multiplicity = estMult;
		edge->meanCoverage = medianCov;
	}

	_uniqueCovThreshold = /*default*/ 2;
	if (!edgesCoverage.empty())
	{
		const float MULT = 1.75f;	//at least 1.75x of mean coverage
		_uniqueCovThreshold = MULT * quantile(edgesCoverage, 75);
	}
	Logger::get().debug() << "Unique coverage threshold " << _uniqueCovThreshold;
}

int MultiplicityInferer::resolveForks()
{
	const int UNIQUE_LEN = (int)Config::get("unique_edge_length");
	const int MAJOR_TO_MINOR = 5;

	int numDisconnected = 0;
	std::vector<GraphNode*> originalNodes(_graph.iterNodes().begin(), 
									      _graph.iterNodes().end());
	for (GraphNode* node : originalNodes)
	{
		//only forks: one in, two out
		if (node->inEdges.size() != 1 ||
			node->outEdges.size() != 2) continue;

		GraphEdge* inEdge = node->inEdges.front();
		GraphEdge* outMajor = node->outEdges[0];
		GraphEdge* outMinor = node->outEdges[1];
		if (outMinor->meanCoverage > outMajor->meanCoverage)
		{
			std::swap(outMajor, outMinor);
		}

		//confirming the correct structure
		if (inEdge->selfComplement || inEdge->isLooped() ||
			outMajor->selfComplement || outMajor->isLooped() ||
			outMinor->selfComplement || outMinor->isLooped()) continue;

		//we want out input edge to be unique. This is not the
		//most reliable way to tell, but at least something
		if (inEdge->length() < UNIQUE_LEN) continue;

		//we want coverage of major edges significantly higher than minor
		if (std::min(outMajor->meanCoverage, inEdge->meanCoverage) < 
			outMinor->meanCoverage * MAJOR_TO_MINOR) continue;

		//looks like all is good
		_graph.disconnectLeft(outMinor);
		_graph.disconnectRight(_graph.complementEdge(outMinor));
		++numDisconnected;
	}

	_aligner.updateAlignments();
	Logger::get().debug() << "[SIMPL] Simplified " << numDisconnected << " forks";
	return numDisconnected;
}

//Masks out edges with low coverage (but not removes them)
int MultiplicityInferer::maskUnsupportedEdges()
{
	const int MIN_CUTOFF = std::round((float)Config::get("min_read_cov_cutoff"));

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	int32_t coverageThreshold = 0;
	
	if (!Parameters::get().unevenCoverage)
	{
		coverageThreshold = std::round((float)this->getMeanCoverage() / 
										Config::get("graph_cov_drop_rate"));
		coverageThreshold = std::max(MIN_CUTOFF, coverageThreshold);
	}
	else
	{
		coverageThreshold = MIN_CUTOFF;
	}
	Logger::get().debug() << "Read coverage cutoff: " << coverageThreshold;

	//std::unordered_set<GraphEdge*> edgesRemove;
	int numMasked = 0;
	for (auto& path : unbranchingPaths)
	{
		if (!path.id.strand()) continue;

		//it's a dead end
		//if (path.nodeRight()->outEdges.size() > 0) continue;

		if (path.meanCoverage < coverageThreshold)
		{
			//Logger::get().debug() << "Low coverage: " 
			//	<< path.edgesStr() << " " << path.meanCoverage;
			for (auto& edge : path.path)
			{
				edge->unreliable = true;
				_graph.complementEdge(edge)->unreliable = true;
				++numMasked;
				//edgesRemove.insert(edge);
				//edgesRemove.insert(_graph.complementEdge(edge));
			}
		}
	}
	//for (auto& edge : edgesRemove) _graph.removeEdge(edge);
	Logger::get().debug() << "[SIMPL] Masked " << numMasked
		<< " edges with low coverage";

	//_aligner.updateAlignments();
	return numMasked;
}

int MultiplicityInferer::removeUnsupportedEdges(bool onlyTips)
{
	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<GraphEdge*> toRemove;
	for (auto& path : unbranchingPaths)
	{
		if (onlyTips && 
			path.nodeLeft()->inEdges.size() > 0 &&
			!path.path.back()->isRightTerminal()) continue;

		bool removePath = true;
		for (auto& edge : path.path)
		{
			if (!edge->unreliable) removePath = false;
		}
		if (removePath)
		{
			for (auto& edge : path.path)
			{
				toRemove.insert(edge);
				toRemove.insert(_graph.complementEdge(edge));
			}
		}
	}

	for (auto& edge : toRemove) _graph.removeEdge(edge);
	Logger::get().debug() << "[SIMPL] Removed " << toRemove.size() / 2
		<< " edges with low coverage";

	_aligner.updateAlignments();
	return toRemove.size() / 2;
}

int MultiplicityInferer::disconnectMinorPaths()
{
	const int DETACH_RATE = 10;

	auto nodeDegree = [](GraphNode* node)
	{
		int maxIn = 0;
		int maxOut = 0;
		for (auto& edge : node->inEdges) 
		{
			if (!edge->isLooped()) maxIn = std::max(maxIn, edge->meanCoverage);
		}
		for (auto& edge : node->outEdges) 
		{
			if (!edge->isLooped()) maxOut = std::max(maxOut, edge->meanCoverage);
		}
		return std::min(maxIn, maxOut);
	};

	int numDisconnected = 0;
	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<FastaRecord::Id> toRemove;
	for (auto& path : unbranchingPaths)
	{
		if (!path.id.strand() || 
			path.isLooped() ||
			path.path.front()->selfComplement) continue;
		if (path.nodeLeft()->inEdges.empty() && 
			path.nodeRight()->outEdges.empty())	continue; //already detached

		bool weakLeft = path.nodeLeft()->inEdges.empty() || 
						nodeDegree(path.nodeLeft()) > path.meanCoverage * DETACH_RATE;
		bool weakRight = path.nodeRight()->outEdges.empty() || 
						 nodeDegree(path.nodeRight()) > path.meanCoverage * DETACH_RATE;
		if (weakLeft && weakRight) toRemove.insert(path.id);
	}
	
	for (auto& path : unbranchingPaths)
	{
		if (toRemove.count(path.id))
		{
			_graph.disconnectLeft(path.path.front());
			_graph.disconnectLeft(_graph.complementEdge(path.path.back()));
			_graph.disconnectRight(path.path.back());
			_graph.disconnectRight(_graph.complementEdge(path.path.front()));
			++numDisconnected;
			//Logger::get().debug() << "Fragile path: " << path.edgesStr();
		}
	}

	_aligner.updateAlignments();
	Logger::get().debug() << "[SIMPL] Disconnected "
		<< numDisconnected << " minor paths";

	return numDisconnected;
}

//Checks each node in the graph if all edges form a single
//connectivity cluster based on read alignment. If
//there are multiple cluster, the node is split into 
//multiple corresponding nodes. This, for example,
//addresses chimeric connections
int MultiplicityInferer::splitNodes()
{
	Logger::get().debug() << "Splitting nodes";
	int numSplit = 0;

	//storing connectivity information
	std::unordered_map<GraphEdge*, 
					   std::unordered_map<GraphEdge*, int>> connections;
	for (auto& readPath : _aligner.getAlignments())
	{
		if (readPath.size() < 2) continue;
		
		for (size_t i = 0; i < readPath.size() - 1; ++i)
		{
			if (readPath[i].edge == readPath[i + 1].edge &&
				readPath[i].edge->isLooped()) continue;
			if (readPath[i].edge->edgeId == 
				readPath[i + 1].edge->edgeId.rc()) continue;

			++connections[readPath[i].edge][readPath[i + 1].edge];
		}
	}

	std::unordered_set<GraphNode*> usedNodes;
	std::vector<GraphNode*> originalNodes(_graph.iterNodes().begin(), 
									      _graph.iterNodes().end());
	for (auto& nodeToSplit : originalNodes)
	{
		if (nodeToSplit->inEdges.size() < 2 ||
			nodeToSplit->outEdges.size() < 2) continue;
		if (usedNodes.count(nodeToSplit)) continue;
		usedNodes.insert(_graph.complementNode(nodeToSplit));
		bool selfComplNode = nodeToSplit == _graph.complementNode(nodeToSplit);

		//initializing sets (to cluster them later)
		typedef SetNode<GraphEdge*> SetElement;
		std::unordered_map<GraphEdge*, SetElement*> edgeToElement;
		SetVec<GraphEdge*> allElements;
		for (GraphEdge* edge : nodeToSplit->inEdges) 
		{
			allElements.push_back(new SetElement(edge));
			edgeToElement[edge] = allElements.back();
		}
		for (GraphEdge* edge : nodeToSplit->outEdges) 
		{
			if (!edge->isLooped())
			{		
				allElements.push_back(new SetElement(edge));
				edgeToElement[edge] = allElements.back();
			}
		}

		//grouping edges if they are connected by reads
		for (GraphEdge* inEdge : nodeToSplit->inEdges)
		{
			for (auto outEdge : connections[inEdge])
			{
				if (outEdge.second >= MIN_JCT_SUPPORT)
				{
					unionSet(edgeToElement[inEdge], 
							 edgeToElement[outEdge.first]);
				}
			}
		}

		auto clusters = groupBySet(allElements);
		if (clusters.size() > 1)	//need to split the node!
		{
			numSplit += 1;
			/*Logger::get().debug() << "Node " 
				<< nodeToSplit->inEdges.size() + nodeToSplit->outEdges.size()
				<< " clusters: " << clusters.size() << " " << selfComplNode;
			for (auto& cl : clusters)
			{
				Logger::get().debug() << "\tCl: " << cl.second.size();
				for (auto edge : cl.second)
				{
					Logger::get().debug() << "\t\t" << edge->edgeId.signedId() << " " 
						<< edge->length() << " " << edge->meanCoverage;
				}
			}*/

			for (auto& cl : clusters)
			{
				auto switchNode = [&nodeToSplit](GraphEdge* edge, 
												 GraphNode* oldNode,
												 GraphNode* newNode)
				{
					if (edge->nodeLeft == oldNode)
					{
						vecRemove(edge->nodeLeft->outEdges, edge);
						edge->nodeLeft = newNode;
						newNode->outEdges.push_back(edge);
					}
					if (edge->nodeRight == oldNode)
					{
						vecRemove(edge->nodeRight->inEdges, edge);
						edge->nodeRight = newNode;
						newNode->inEdges.push_back(edge);
					}

				};

				GraphNode* newNode = _graph.addNode();
				GraphNode* newComplNode = _graph.addNode();
				for (GraphEdge* edge : cl.second)
				{
					GraphEdge* complEdge = _graph.complementEdge(edge);
					GraphNode* complSplit = _graph.complementNode(nodeToSplit);
					switchNode(edge, nodeToSplit, newNode);
					if (!edge->selfComplement && !selfComplNode)
					{
						switchNode(complEdge, complSplit, newComplNode);
					}
				}
			}
		}
	}

	_aligner.updateAlignments();
	Logger::get().debug() << "[SIMPL] Split " << numSplit << " nodes";

	return numSplit;
}


//Disconnects edges, which had low number of reads that connect them
//with the rest of the graph. #of reads is relative to the
//edge coverage
int MultiplicityInferer::removeUnsupportedConnections()
{
	std::unordered_map<GraphEdge*, int32_t> rightConnections;
	std::unordered_map<GraphEdge*, int32_t> leftConnections;

	for (auto& readPath : _aligner.getAlignments())
	{
		if (readPath.size() < 2) continue;
		//int overhang = std::max(readPath.front().overlap.curBegin,
		//						readPath.back().overlap.curLen - 
		//							readPath.back().overlap.curEnd);
		//if (overhang > (int)Config::get("maximum_overhang")) continue;

		for (size_t i = 0; i < readPath.size() - 1; ++i)
		{
			if (readPath[i].edge == readPath[i + 1].edge &&
				readPath[i].edge->isLooped()) continue;
			if (readPath[i].edge->edgeId == 
				readPath[i + 1].edge->edgeId.rc()) continue;

			++rightConnections[readPath[i].edge];
			++leftConnections[readPath[i + 1].edge];
			GraphEdge* complLeft = _graph.complementEdge(readPath[i].edge);
			GraphEdge* complRight = _graph.complementEdge(readPath[i + 1].edge);
			++rightConnections[complRight];
			++leftConnections[complLeft];
		}
	}

	int numDisconnected = 0;
	for (auto& edge : _graph.iterEdges())
	{
		if (!edge->edgeId.strand() || edge->isLooped()) continue;
		GraphEdge* complEdge = _graph.complementEdge(edge);

		//int32_t coverageThreshold = edge->meanCoverage / 
		//						Config::get("graph_cov_drop_rate");
		//coverageThreshold = std::max(MIN_JCT_SUPPORT, coverageThreshold);
		int32_t coverageThreshold = MIN_JCT_SUPPORT;

		//Logger::get().debug() << "Adjacencies: " << edge->edgeId.signedId() << " "
		//	<< leftConnections[edge] / 2 << " " << rightConnections[edge] / 2;

		if (!edge->nodeRight->isEnd() &&
			edge->nodeRight->isBifurcation() &&
			rightConnections[edge] / 2 < coverageThreshold)
		{
			++numDisconnected;
			//Logger::get().debug() << "Chimeric right: " <<
			//	edge->edgeId.signedId() << " " << rightConnections[edge] / 2;

			_graph.disconnectRight(edge);
			_graph.disconnectLeft(complEdge);

			if (edge->selfComplement) continue;	//already discinnected
		}
		if (!edge->nodeLeft->isEnd() &&
			edge->nodeLeft->isBifurcation() &&
			leftConnections[edge] / 2 < coverageThreshold)
		{
			++numDisconnected;
			//Logger::get().debug() << "Chimeric left: " <<
			//	edge->edgeId.signedId() << " " << leftConnections[edge] / 2;

			_graph.disconnectLeft(edge);
			_graph.disconnectRight(complEdge);
		}
	}

	Logger::get().debug() << "[SIMPL] Disconnected " << numDisconnected << " edges";

	_aligner.updateAlignments();
	return numDisconnected;
}

//This function collapses simple loops:
//1. One loop edge with one entrance and one exit
//2. Loop length is shorter than lengths of entrance/exit
//3. Loop coverage is roughly equal or less than coverage of entrance/exit
int MultiplicityInferer::collapseHeterozygousLoops(bool removeAlternatives)
{
	const float COV_MULT = 1.5;

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<FastaRecord::Id> toUnroll;
	std::unordered_set<FastaRecord::Id> toRemove;
	int numMasked = 0;
	for (auto& loop : unbranchingPaths)
	{
		if (!loop.id.strand()) continue;
		if (!loop.isLooped()) continue;
		if (loop.path.front()->selfComplement) continue;

		GraphNode* node = loop.nodeLeft();
		if (node->inEdges.size() != 2 ||
			node->outEdges.size() != 2) continue;

		UnbranchingPath* entrancePath = nullptr;
		UnbranchingPath* exitPath = nullptr;
		for (auto& cand : unbranchingPaths)
		{
			if (cand.nodeRight() == node &&
				loop.id != cand.id) entrancePath = &cand;
			if (cand.nodeLeft() == node &&
				loop.id != cand.id) exitPath = &cand;
		}

		if (entrancePath->isLooped()) continue;
		if (entrancePath->id == exitPath->id.rc()) continue;

		//loop coverage should be roughly equal or less
		if (loop.meanCoverage > 
				COV_MULT * std::min(entrancePath->meanCoverage, 
									entrancePath->meanCoverage)) continue;

		//loop should not be longer than other branches
		if (loop.length > std::max(entrancePath->length, 
								   exitPath->length)) continue;

		if (!loop.path.front()->altHaplotype) ++numMasked;
		for (auto& edge : loop.path)
		{
			edge->altHaplotype = true;
			_graph.complementEdge(edge)->altHaplotype = true;
		}
		//either remove or unroll loop, depending on the coverage
		if (loop.meanCoverage < 
			(entrancePath->meanCoverage + exitPath->meanCoverage) / 4)
		{
			toRemove.insert(loop.id);
			toRemove.insert(loop.id.rc());
		}
		else
		{
			toUnroll.insert(loop.id);
			toUnroll.insert(loop.id.rc());
		}
	}

	if (removeAlternatives)
	{
		for (auto& path : unbranchingPaths)
		{
			if (toUnroll.count(path.id))
			{
				//Logger::get().debug() << "Unrolled loop: " << path.edgesStr();

				GraphNode* newNode = _graph.addNode();
				size_t id = path.nodeLeft()->inEdges[0] == path.path.back();
				GraphEdge* prevEdge = path.nodeLeft()->inEdges[id];

				vecRemove(path.nodeLeft()->outEdges, path.path.front());
				vecRemove(path.nodeLeft()->inEdges, prevEdge);
				path.nodeLeft() = newNode;
				newNode->outEdges.push_back(path.path.front());
				prevEdge->nodeRight = newNode;
				newNode->inEdges.push_back(prevEdge);
			}
			if (toRemove.count(path.id))
			{
				//Logger::get().debug() << "Removed loop: " << path.edgesStr();

				GraphNode* newLeft = _graph.addNode();
				GraphNode* newRight = _graph.addNode();

				vecRemove(path.nodeLeft()->outEdges, path.path.front());
				vecRemove(path.nodeLeft()->inEdges, path.path.back());
				path.nodeLeft() = newLeft;
				newRight->inEdges.push_back(path.path.back());
				path.nodeRight() = newRight;
				newLeft->outEdges.push_back(path.path.front());
			}
		}

		Logger::get().debug() << "[SIMPL] Removed " << (toRemove.size() + toUnroll.size()) / 2
			<< " heterozygous loops";
		_aligner.updateAlignments();
		return (toRemove.size() + toUnroll.size()) / 2;
	}
	else
	{
		Logger::get().debug() << "[SIMPL] Masked " << numMasked << " heterozygous loops";
		return numMasked;
	}

}

void MultiplicityInferer::trimTipsIteration(int& outShort, int& outLong)
{
	const int SHORT_TIP = Config::get("short_tip_length");
	const int LONG_TIP = Config::get("long_tip_length");
	const int COV_RATE = 2;
	const int LEN_RATE = 10;

	std::unordered_set<FastaRecord::Id> toRemove;
	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();
	std::unordered_map<GraphEdge*, UnbranchingPath*> ubIndex;
	for (auto& path : unbranchingPaths)
	{
		for (auto& edge: path.path) ubIndex[edge] = &path;
	}

	int shortClipped = 0;
	int longClipped = 0;

	for (auto& tipPath : unbranchingPaths)
	{
		if (!tipPath.path.back()->isRightTerminal()) continue;	//right-tip
		if (tipPath.nodeLeft()->outEdges.size() == 1) continue;	//already detached from the left
		if (tipPath.path.front()->selfComplement) continue;		//never-ever!

		//short tip, remove regardless of coverage
		if (tipPath.length < SHORT_TIP)
		{
			toRemove.insert(tipPath.id);
			++shortClipped;
			continue;
		}

		//longer then max long tip, continue
		if (tipPath.length > LONG_TIP) continue;

		//tip longer than short, and shorter than long :)
		//need to check the graph structure and coverage
		
		//get "true path" entrance and exit. There must be
		//exactly one of each. True edges should be of sufficient
		//length and/or continue into the rest of the graph
		GraphNode* tipNode = tipPath.nodeLeft();
		std::vector<UnbranchingPath*> entrances;
		for (GraphEdge* edge : tipNode->inEdges)
		{
			UnbranchingPath& path = *ubIndex[edge];
			if (path.path.back() == edge)
			{
				if (path.length > LEN_RATE * tipPath.length ||
					path.nodeLeft()->inEdges.size() > 0) entrances.push_back(&path);
			}
		}
		std::vector<UnbranchingPath*> exits;
		for (GraphEdge* edge : tipNode->outEdges)
		{
			UnbranchingPath& path = *ubIndex[edge];
			if (path.path.front() == edge)
			{
				if (path.length > LEN_RATE * tipPath.length ||
					path.nodeRight()->outEdges.size() > 0) exits.push_back(&path);
			}
		}
		if (entrances.size() != 1 || exits.size() != 1) continue;

		//remove the tip if its coverage or length is
		//significantly lower than the true path's
		int trueCov = std::min(entrances.front()->meanCoverage, 
					 		   exits.front()->meanCoverage);
		int trueLen = std::min(entrances.front()->length, exits.front()->length);
		if (trueCov > COV_RATE * tipPath.meanCoverage ||
			trueLen > LEN_RATE * tipPath.length)
		{
			toRemove.insert(tipPath.id);
			++longClipped;
		}
	}
	
	for (auto& path : unbranchingPaths)
	{
		if (toRemove.count(path.id))
		{
			//Logger::get().debug() << "Tip " << path.edgesStr() 
			//	<< " len:" << path.length << " cov:" << path.meanCoverage;

			GraphEdge* targetEdge = path.path.front();
			GraphEdge* complEdge = _graph.complementEdge(targetEdge);

			vecRemove(targetEdge->nodeLeft->outEdges, targetEdge);
			targetEdge->nodeLeft = _graph.addNode();
			targetEdge->nodeLeft->outEdges.push_back(targetEdge);

			//if (targetEdge->selfComplement) continue;

			vecRemove(complEdge->nodeRight->inEdges, complEdge);
			complEdge->nodeRight = _graph.addNode();
			complEdge->nodeRight->inEdges.push_back(complEdge);
		}
	}
	_aligner.updateAlignments();
	outShort = shortClipped;
	outLong = longClipped;
}

//This function collapses simply bubbles caused by
//alternative haplotypes / strains. They are defined as follows:
//1. Structure: 1 input, 2 branches, 1 output: -<>-
//2. Size of each branch is shorter than MAX_BUBBLE_LEN below
//3. Total coverage of bubbles branches roughly equasl to input/output coverages
//4. Each branch is shorter than both entrace and exits. We need this to
//   distinguish from the case of two repeats of multiplicity 2
//Note that we are not using any global coverage assumptions here.
int MultiplicityInferer::collapseHeterozygousBulges(bool removeAlternatives)
{
	const float MAX_COV_VAR = 1.5;
	const int MAX_BUBBLE_LEN = Config::get("max_bubble_length");

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<FastaRecord::Id> toSeparate;
	int numMasked = 0;
	for (auto& path : unbranchingPaths)
	{
		if (path.isLooped()) continue;

		std::vector<UnbranchingPath*> twoPaths;
		for (auto& candEdge : unbranchingPaths)
		{
			if (candEdge.nodeLeft() == path.nodeLeft() &&
				candEdge.nodeRight() == path.nodeRight()) 
			{
				twoPaths.push_back(&candEdge);
			}
		}

		//making sure the structure is ok
		if (twoPaths.size() != 2) continue;
		if (twoPaths[0]->id == twoPaths[1]->id.rc()) continue;
		if (toSeparate.count(twoPaths[0]->id) || 
			toSeparate.count(twoPaths[1]->id)) continue;
		if (twoPaths[0]->nodeLeft()->inEdges.size() != 1 ||
			twoPaths[0]->nodeLeft()->outEdges.size() != 2 ||
			twoPaths[0]->nodeRight()->outEdges.size() != 1 ||
			twoPaths[0]->nodeRight()->inEdges.size() != 2) continue;

		UnbranchingPath* entrancePath = nullptr;
		UnbranchingPath* exitPath = nullptr;
		for (auto& cand : unbranchingPaths)
		{
			if (cand.nodeRight() == 
				twoPaths[0]->nodeLeft()) entrancePath = &cand;
			if (cand.nodeLeft() == twoPaths[0]->nodeRight()) exitPath = &cand;
		}

		//sanity check for maximum bubble size
		if (std::max(twoPaths[0]->length, twoPaths[1]->length) > 
			MAX_BUBBLE_LEN) continue;

		//coverage requirement: sum over two branches roughly equals to
		//exit and entrance coverage or less
		float covSum = twoPaths[0]->meanCoverage + twoPaths[1]->meanCoverage;
		if (covSum > std::min(entrancePath->meanCoverage * MAX_COV_VAR,
							  exitPath->meanCoverage * MAX_COV_VAR)) continue;

		//require bubble branches to be shorter than entrance or exit,
		//to distinguish from the case of two consecutive repeats
		//of multiplicity 2
		if (std::max(twoPaths[0]->length, twoPaths[1]->length) >
			std::max(entrancePath->length, exitPath->length)) continue;

		if (twoPaths[0]->meanCoverage > twoPaths[1]->meanCoverage)
		{
			std::swap(twoPaths[0], twoPaths[1]);
		}

		if (!twoPaths[0]->path.front()->altHaplotype ||
			!twoPaths[1]->path.front()->altHaplotype) ++numMasked;

		for (size_t i = 0; i < 2; ++i)
		{
			for (auto& edge : twoPaths[i]->path)
			{
				edge->altHaplotype = true;
				_graph.complementEdge(edge)->altHaplotype = true;
			}
		}

		if (removeAlternatives)
		{
			toSeparate.insert(twoPaths[0]->id);
			toSeparate.insert(twoPaths[0]->id.rc());
			for (auto& edge : twoPaths[1]->path)
			{
				edge->meanCoverage += twoPaths[0]->meanCoverage;
				_graph.complementEdge(edge)->meanCoverage += twoPaths[0]->meanCoverage;
				edge->altHaplotype = false;
				_graph.complementEdge(edge)->altHaplotype = false;
			}
		}
	}

	if (removeAlternatives)
	{
		for (auto& path : unbranchingPaths)
		{
			if (toSeparate.count(path.id))
			{
				//Logger::get().debug() << "Seperated branch: " << path.edgesStr();

				GraphNode* newLeft = _graph.addNode();
				GraphNode* newRight = _graph.addNode();
				vecRemove(path.nodeLeft()->outEdges, path.path.front());
				vecRemove(path.nodeRight()->inEdges, path.path.back());
				path.nodeLeft() = newLeft;
				path.nodeRight() = newRight;
				newLeft->outEdges.push_back(path.path.front());
				newRight->inEdges.push_back(path.path.back());
			}
		}

		Logger::get().debug() << "[SIMPL] Removed " << toSeparate.size() / 2 
			<< " heterozygous bulges";

		_aligner.updateAlignments();
		return toSeparate.size() / 2;
	}
	else
	{
		Logger::get().debug() << "[SIMPL] Masked " << numMasked
			<< " heterozygous bulges";
		return numMasked;
	}
}
