#include "duckdb/parser/transformer.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

void Transformer::AddPivotEntry(string enum_name, unique_ptr<SelectNode> base, string column_name) {
	if (parent) {
		parent->AddPivotEntry(std::move(enum_name), std::move(base), std::move(column_name));
		return;
	}
	auto result = make_unique<CreatePivotEntry>();
	result->enum_name = std::move(enum_name);
	result->base = std::move(base);
	result->column_name = std::move(column_name);

	pivot_entries.push_back(std::move(result));
}

bool Transformer::HasPivotEntries() {
	if (parent) {
		return parent->HasPivotEntries();
	}
	return !pivot_entries.empty();
}

idx_t Transformer::PivotEntryCount() {
	if (parent) {
		return parent->PivotEntryCount();
	}
	return pivot_entries.size();
}

unique_ptr<SQLStatement> Transformer::GenerateCreateEnumStmt(unique_ptr<CreatePivotEntry> entry) {
	auto result = make_unique<CreateStatement>();
	auto info = make_unique<CreateTypeInfo>();

	info->temporary = true;
	info->internal = false;
	info->catalog = INVALID_CATALOG;
	info->schema = INVALID_SCHEMA;
	info->name = std::move(entry->enum_name);
	info->on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;

	// generate the query that will result in the enum creation
	auto select_node = std::move(entry->base);
	auto columnref = make_unique<ColumnRefExpression>(std::move(entry->column_name));
	auto cast = make_unique<CastExpression>(LogicalType::VARCHAR, columnref->Copy());
	select_node->select_list.push_back(std::move(cast));

	// order by the column
	auto modifier = make_unique<OrderModifier>();
	modifier->orders.emplace_back(OrderType::ASCENDING, OrderByNullType::ORDER_DEFAULT, std::move(columnref));
	select_node->modifiers.push_back(std::move(modifier));

	auto select = make_unique<SelectStatement>();
	select->node = std::move(select_node);
	info->query = std::move(select);
	info->type = LogicalType::INVALID;

	result->info = std::move(info);
	return std::move(result);
}

// unique_ptr<SQLStatement> GenerateDropEnumStmt(string enum_name) {
//	auto result = make_unique<DropStatement>();
//	result->info->if_exists = true;
//	result->info->schema = INVALID_SCHEMA;
//	result->info->catalog = INVALID_CATALOG;
//	result->info->name = std::move(enum_name);
//	result->info->type = CatalogType::TYPE_ENTRY;
//	return std::move(result);
//}

unique_ptr<SQLStatement> Transformer::CreatePivotStatement(unique_ptr<SQLStatement> statement) {
	auto result = make_unique<MultiStatement>();
	for (auto &pivot : pivot_entries) {
		result->statements.push_back(GenerateCreateEnumStmt(std::move(pivot)));
	}
	result->statements.push_back(std::move(statement));
	// FIXME: drop the types again!?
	//	for(auto &pivot : pivot_entries) {
	//		result->statements.push_back(GenerateDropEnumStmt(std::move(pivot->enum_name)));
	//	}
	return std::move(result);
}

unique_ptr<QueryNode> Transformer::TransformPivotStatement(duckdb_libpgquery::PGSelectStmt *stmt) {
	auto pivot = stmt->pivot;
	auto source = TransformTableRefNode(pivot->source);
	auto columns = TransformPivotList(pivot->columns);

	auto select_node = make_unique<SelectNode>();
	// handle the CTEs
	if (stmt->withClause) {
		TransformCTE(reinterpret_cast<duckdb_libpgquery::PGWithClause *>(stmt->withClause), select_node->cte_map);
	}

	// generate CREATE TYPE statements for each of the columns that do not have an IN list
	auto pivot_idx = PivotEntryCount();
	for (idx_t c = 0; c < columns.size(); c++) {
		auto &col = columns[c];
		if (!col.pivot_enum.empty() || !col.entries.empty()) {
			continue;
		}
		if (col.names.size() != 1) {
			throw InternalException("PIVOT statement with multiple names in pivot entry!?");
		}
		auto enum_name = "__pivot_enum_" + std::to_string(pivot_idx) + "_" + std::to_string(c);

		auto new_select = make_unique<SelectNode>();
		ExtractCTEsRecursive(new_select->cte_map);
		new_select->from_table = source->Copy();
		AddPivotEntry(enum_name, std::move(new_select), col.names[0]);
		col.pivot_enum = enum_name;
	}

	// generate the actual query, including the pivot
	select_node->select_list.push_back(make_unique<StarExpression>());

	auto pivot_ref = make_unique<PivotRef>();
	pivot_ref->source = std::move(source);
	if (pivot->aggrs) {
		TransformExpressionList(*pivot->aggrs, pivot_ref->aggregates);
	} else {
		if (!pivot->unpivots) {
			throw InternalException("No unpivots and no aggrs");
		}
		pivot_ref->unpivot_names = TransformStringList(pivot->unpivots);
	}
	if (pivot->groups) {
		pivot_ref->groups = TransformStringList(pivot->groups);
	}
	pivot_ref->pivots = std::move(columns);
	select_node->from_table = std::move(pivot_ref);
	return std::move(select_node);
}

} // namespace duckdb
