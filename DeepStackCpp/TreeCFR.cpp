#include "TreeCFR.h"
#include <iostream>
#include <string>


TreeCFR::TreeCFR(){}


TreeCFR::~TreeCFR()
{
	for (auto itr = _cached_terminal_equities.begin(); itr != _cached_terminal_equities.end(); itr++)
	{
		delete itr->second;
	}

	_cached_terminal_equities.clear();
}

void TreeCFR::run_cfr(Node& root, const ArrayXX& starting_ranges, size_t iter_count, size_t skip_iters)
{
	_cfr_skip_iters = skip_iters;
	Util::Print(starting_ranges);

	assert(starting_ranges.size() > 0);
	assert(iter_count >= _cfr_skip_iters);
	iter_count = iter_count > 0 ? iter_count : cfr_iters;
	root.ranges = starting_ranges;

	for (size_t iter = 0; iter < iter_count; iter++)
	{
		cfrs_iter_dfs(root, iter);
	}
}


void TreeCFR::cfrs_iter_dfs(Node& node, size_t iter)
{
	assert(node.current_player == P1 || node.current_player == P2 || node.current_player == chance);
	//--compute values using terminal_equity in terminal nodes
	if (node.terminal)
	{
		_fillCFvaluesForTerminalNode(node);
	}
	else
	{
		_fillCFvaluesForNonTerminalNode(node, iter);
	}
}

void TreeCFR::_fillCFvaluesForTerminalNode(Node &node)
{
	assert(node.terminal && (node.type == terminal_fold || node.type == terminal_call));
	int opponnent = 1 - node.current_player;

	terminal_equity* termEquity = _get_terminal_equity(node);

	// CF values  2p X each private hand.
	node.cf_values = ArrayXX::Zero(players_count, card_count);

	if (node.type == terminal_fold)
	{
		termEquity->tree_node_fold_value(node.ranges, node.cf_values, opponnent);
	}
	else
	{
		termEquity->tree_node_call_value(node.ranges, node.cf_values);
	}

	//--multiply by the pot
	node.cf_values *= node.pot;

	//Looks like this not needed? ToDo: check during debugging!
	//node.cf_values.conservativeResize(node.ranges_absolute.rows(), node.ranges_absolute.cols());
}


void TreeCFR::_fillCFvaluesForNonTerminalNode(Node &node, size_t iter)
{
	const int actions_count = (int)node.children.size();

	ArrayXX current_strategy;


	//--current cfv[[actions, players, ranges]]
	//	Tensor<float, 3> cf_values_allactions(actions_count, players_count, card_count);
	//[actions_count x card_count][players_count]
	Array<float, Dynamic, card_count>  cf_values_allactions[players_count];
	cf_values_allactions[P1] = Array<float, Dynamic, card_count>(actions_count, card_count);
	cf_values_allactions[P2] = Array<float, Dynamic, card_count>(actions_count, card_count);

	//map <player/[actions, ranges]>
	map <int, ArrayXX> children_ranges_absolute;

	static const int PLAYERS_DIM = 1;

	//cf_values_allactions.setZero();

	if (node.current_player == chance)
	{
		_fillChanceRangesAndStrategy(node, children_ranges_absolute, current_strategy);
	}
	else
	{
		_fillPlayersRangesAndStrategy(node, children_ranges_absolute, current_strategy);
	}

	for (size_t i = 0; i < node.children.size(); i++)
	{
		Node* child_node = node.children[i];
		//--set new absolute ranges(after the action) for the child
		child_node->ranges = ArrayXX(node.ranges);

		child_node->ranges.row(P1) = children_ranges_absolute[P1].row(i);
		child_node->ranges.row(P2) = children_ranges_absolute[P2].row(i);
		cfrs_iter_dfs(*child_node, iter);

		// Now coping cf_values from children to calculate the regret
		cf_values_allactions[P1].row(i) = child_node->cf_values.row(P1); //ToDo: Can be single copy operation(two rows copy)?
		cf_values_allactions[P2].row(i) = child_node->cf_values.row(P2);

		//cf_values_allactions.chip(i, 0) = Util::ToTensor(child_node->cf_values);
	}

	node.cf_values = MatrixX(players_count, card_count);
	node.cf_values.fill(0);

	Tensor<float, 2> tempVar;

	if (node.current_player == chance)
	{
		// For the chance node just sum regrets from all cards
		node.cf_values.row(P1) = cf_values_allactions[P1].colwise().sum();
		node.cf_values.row(P2) = cf_values_allactions[P2].colwise().sum();
		//node.cf_values.row(0) = Util::TensorToArray2d(cf_values_allactions, 0, PLAYERS_DIM, tempVar).colwise().sum();
		//node.cf_values.row(1) = Util::TensorToArray2d(cf_values_allactions, 1, PLAYERS_DIM, tempVar).colwise().sum();
	}
	else
	{
		ArrayXX current_regrets = ComputeRegrets(node, current_strategy, cf_values_allactions);
		update_regrets(node, current_regrets);
		//--accumulating average strategy
		update_average_strategy(node, current_strategy, iter);
	}
}

terminal_equity* TreeCFR::_get_terminal_equity(Node& node)
{
	auto it = _cached_terminal_equities.find(&node.board);

	terminal_equity* cached = nullptr;
	if (it == _cached_terminal_equities.end())
	{
		cached = new terminal_equity();
		cached->set_board(node.board);
		_cached_terminal_equities[&node.board] = cached;
	}
	else
	{
		cached = it->second;
	}

	return cached;
}

void TreeCFR::update_regrets(Node& node, const ArrayXX& current_regrets)
{
	//--node.regrets:add(current_regrets)
	//	--local negative_regrets = node.regrets[node.regrets:lt(0)]
	//	--node.regrets[node.regrets:lt(0)] = negative_regrets
	node.regrets.array() += current_regrets;

	node.regrets = (node.regrets.array() >= regret_epsilon).select(
		node.regrets,
		MatrixXd::Constant(node.regrets.rows(), node.regrets.cols(), regret_epsilon));
}

void TreeCFR::update_average_strategy(Node& node, ArrayXX& current_strategy, size_t iter)
{
	if (iter >= _cfr_skip_iters)
	{
		if (node.strategy.size() == 0)
		{
			const int actions_count = (int)node.children.size();
			node.strategy = ArrayXX::Zero(actions_count, card_count);
		}

		if (node.iter_weight_sum.size() == 0)
		{
			node.iter_weight_sum = ArrayX::Zero(card_count);
		}

		ArrayX iter_weight_contribution = node.ranges.row(node.current_player); 
		Util::ClipLow(iter_weight_contribution, regret_epsilon);
		node.iter_weight_sum += iter_weight_contribution;

		ArrayXX iter_weight = iter_weight_contribution / node.iter_weight_sum;
		iter_weight.resize(1, card_count);
		ArrayXX expanded_weight = Util::ExpandAs(iter_weight, node.strategy);
		ArrayXX old_strategy_scale = (expanded_weight * (-1)) + 1; //--same as 1 - expanded weight
		node.strategy *= old_strategy_scale;
		node.strategy += (current_strategy  * expanded_weight);
	}
}

void TreeCFR::_fillChanceRangesAndStrategy(Node &node, map<int, ArrayXX> &children_ranges_absolute, ArrayXX& current_strategy)
{
	int actions_count = (int)node.children.size();
	current_strategy = node.strategy;

	ArrayXX ranges_mul_matrix = node.ranges.row(0).replicate(actions_count, 1);
	children_ranges_absolute[0] = current_strategy * ranges_mul_matrix;

	ranges_mul_matrix = node.ranges.row(1).replicate(actions_count, 1);
	children_ranges_absolute[1] = current_strategy * ranges_mul_matrix;	
}

void TreeCFR::_fillPlayersRangesAndStrategy(Node &node, map<int, ArrayXX> &children_ranges_absolute, ArrayXX& current_strategy)
{
	int currentPlayer = node.current_player; // Because we have zero based indexes, unlike the original source.
	int opponentIndex = 1 - currentPlayer;

	const int actions_count = (int)node.children.size();

	//--we have to compute current strategy at the beginning of each iteration

	//--initialize regrets in the first iteration
	if (node.regrets.size() == 0)
	{
		node.regrets = MatrixX(actions_count, card_count);
		node.regrets.fill(regret_epsilon);
	}

	assert((node.regrets >= regret_epsilon).all() && "All regrets must be positive or uncomment commented code below.");

	//else
	//{
	//	//--leave positive regrets only so that we can compute the current strategy from them
	//	Util::ClipLow(node.regrets, regret_epsilon);
	//}

	//--compute the current strategy
	ArrayXX regrets_sum = node.regrets.colwise().sum().row(action_dimension);
	current_strategy = ArrayXX(node.regrets);
	current_strategy /= Util::ExpandAs(regrets_sum, current_strategy); // We are dividing regrets for each actions by the sum of regrets for all actions and doing this element wise for every card

	ArrayXX ranges_mul_matrix = node.ranges.row(currentPlayer).replicate(actions_count, 1);
	children_ranges_absolute[currentPlayer] = current_strategy.array() * ranges_mul_matrix.array(); // Just multiplying ranges(cards probabilities) by the probability that action will be taken(from the strategy) inside the matrix  
	children_ranges_absolute[opponentIndex] = node.ranges.row(opponentIndex).replicate(actions_count, 1); //For opponent we are just cloning ranges
}

ArrayXX TreeCFR::ComputeRegrets(Node &node, ArrayXX &current_strategy, Array<float, Dynamic, card_count>* cf_values_allactions)
{
	Tensor<float, 2> tempVar;
	static const int PLAYERS_DIM = 1;
	const int actions_count = (int)node.children.size();
	const int currentPlayer = node.current_player; // Because we have zero based indexes, unlike the original source.
	const int opponent = 1 - node.current_player;

	assert(current_strategy.rows() == actions_count && current_strategy.cols() == card_count);
	//current_strategy.conservativeResize(actions_count, card_count); // Do we need this?

	//Map<ArrayXX> opCfAr = Util::TensorToArray2d(cf_values_allactions, opponentIndexNorm, PLAYERS_DIM, tempVar);

	ArrayXX opCfAr = cf_values_allactions[opponent]; // [actions X cards] - cf values for the opponent
	node.cf_values.row(opponent) = opCfAr.colwise().sum(); // for opponent assume that strategy is uniform

	//ArrayXX strategy_mul_matrix = current_strategy; //ToDo: remove or add copy
	//Map<ArrayXX> plCfAr = Util::TensorToArray2d(cf_values_allactions, currentPlayerNorm, PLAYERS_DIM, tempVar);

	ArrayXX currentPlayerCfValues = cf_values_allactions[currentPlayer];
	assert(currentPlayerCfValues.rows() == actions_count && currentPlayerCfValues.cols() == card_count);

	ArrayXX weigtedCfValues = current_strategy * currentPlayerCfValues; // weight the regrets by the used strategy
	node.cf_values.row(currentPlayer) = weigtedCfValues.colwise().sum(); // summing CF values for different actions

	//--computing regrets
	//Map<ArrayXX> current_regrets = Util::TensorToArray2d(cf_values_allactions, currentPlayerNorm, PLAYERS_DIM, tempVar);

	//current_regrets.resize(actions_count, card_count); Do we need this resize?


	ArrayXX cfValuesOdCurrentPlayer = node.cf_values.row(currentPlayer);
	cfValuesOdCurrentPlayer.resize(1, card_count); // [1(action) X card_count]
	ArrayXX matrixToSubstract = cfValuesOdCurrentPlayer.replicate(actions_count, 1); // [actions X card_count]
	currentPlayerCfValues -= matrixToSubstract; // Substructing sum of CF values over all actions with every action CF value
	return currentPlayerCfValues;
}



