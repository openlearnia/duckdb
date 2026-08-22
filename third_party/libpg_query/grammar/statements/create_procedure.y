/*****************************************************************************
 *
 * CREATE PROCEDURE stmt
 *
 *****************************************************************************/
CreateProcedureStmt:
		CREATE_P PROCEDURE qualified_name param_list RETURNS Typename LANGUAGE ColIdOrString AS Sconst
			{
				PGCreateProcedureStmt *n = makeNode(PGCreateProcedureStmt);
				n->name = $3;
				n->params = $4;
				n->returnType = $6;
				n->language = $8;
				n->body = $10;
				n->onconflict = PG_ERROR_ON_CONFLICT;
				$$ = (PGNode *)n;
			}
		| CREATE_P OR REPLACE PROCEDURE qualified_name param_list RETURNS Typename LANGUAGE ColIdOrString AS Sconst
			{
				PGCreateProcedureStmt *n = makeNode(PGCreateProcedureStmt);
				n->name = $5;
				n->params = $6;
				n->returnType = $8;
				n->language = $10;
				n->body = $12;
				n->onconflict = PG_REPLACE_ON_CONFLICT;
				$$ = (PGNode *)n;
			}
	;
