/*****************************************************************************
 * REFRESH MATERIALIZED VIEW [IF STALE] qualified_name
 *****************************************************************************/
RefreshMatViewStmt:
		REFRESH MATERIALIZED VIEW qualified_name
				{
					PGRefreshMatViewStmt *n = makeNode(PGRefreshMatViewStmt);
					n->relation = $4;
					n->if_stale = false;
					$$ = (PGNode *) n;
				}
		| REFRESH MATERIALIZED VIEW IF_P STALE qualified_name
				{
					PGRefreshMatViewStmt *n = makeNode(PGRefreshMatViewStmt);
					n->relation = $6;
					n->if_stale = true;
					$$ = (PGNode *) n;
				}
		;

